#include "UIManager.hpp"
#include "app/PDAController.hpp"
#include "core/Config.hpp"
#include "core/Glyphs.hpp"
#include "core/Logger.hpp"
#include "screens/Screen.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <imgui.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#endif

namespace {

namespace fs = std::filesystem;

// Tracks availability of Python/Pip/Pillow for atlas baking
struct BakeEnvironmentStatus {
    bool script_found = false;
    bool python_found = false;
    bool pip_found = false;
    bool pillow_found = false;
    std::string python_invocation;
};

std::vector<std::string> GetVisibleHomeSections(const YipOS::Config& config) {
    std::vector<std::string> visible;
    auto labels = YipOS::Glyphs::GetDefaultHomeSectionLabels();
    visible.reserve(labels.size());
    for (const auto& label : labels) {
        if (config.GetState(YipOS::Glyphs::GetHomeSectionStateKey(label), "1") != "0") {
            visible.push_back(label);
        }
    }
    return visible;
}

std::string SerializeHomeSections(const std::vector<std::string>& labels) {
    if (labels.empty()) return {};
    std::string joined = labels[0];
    for (size_t i = 1; i < labels.size(); ++i) {
        joined += ',';
        joined += labels[i];
    }
    return joined;
}

fs::path GetMacroAtlasScriptPath(const std::string& assets_path) {
    std::error_code ec;
    fs::path assets = assets_path.empty() ? fs::path("assets") : fs::path(assets_path);
    if (assets.is_relative()) {
        assets = fs::absolute(assets, ec);
        if (ec) return {};
    }
    assets = assets.lexically_normal();

    fs::path current = fs::current_path(ec);
    if (ec) return {};

    std::vector<fs::path> candidates = {
        assets.parent_path().parent_path() / "generate_macro_atlas.py",
        assets.parent_path() / "generate_macro_atlas.py",
        current / "generate_macro_atlas.py",
    };

    for (const auto& candidate : candidates) {
        fs::path normalized = candidate.lexically_normal();
        if (!normalized.empty() && fs::exists(normalized)) return normalized;
    }
    return {};
}

fs::path GetBakedAtlasOutputPath(const std::string& config_path) {
    std::error_code ec;
    fs::path base = config_path.empty() ? fs::current_path(ec) : fs::path(config_path).parent_path();
    if (ec) base = fs::path("exports");
    return base / "exports" / "WilliamsTube_MacroAtlas_home_baked.png";
}

bool CommandSucceeds(const std::string& command) {
#ifdef _WIN32
    std::string full_command = command + " >nul 2>&1";
    return std::system(full_command.c_str()) == 0;
#else
    std::string full_command = command + " >/dev/null 2>&1";
    int result = std::system(full_command.c_str());
    return result != -1 && WIFEXITED(result) && WEXITSTATUS(result) == 0;
#endif
}

BakeEnvironmentStatus DetectBakeEnvironment(const std::string& assets_path) {
    BakeEnvironmentStatus best_status;
    best_status.script_found = !GetMacroAtlasScriptPath(assets_path).empty();
    int best_score = -1;

#ifdef _WIN32
    const std::vector<std::string> python_candidates = {"py -3", "python"};
#else
    const std::vector<std::string> python_candidates = {"python3", "python"};
#endif

    for (const auto& candidate : python_candidates) {
        BakeEnvironmentStatus candidate_status;
        candidate_status.script_found = best_status.script_found;

        if (CommandSucceeds(candidate + " -c \"import sys\"")) {
            candidate_status.python_found = true;
            candidate_status.python_invocation = candidate;
            candidate_status.pip_found = CommandSucceeds(candidate + " -m pip --version");
            candidate_status.pillow_found = CommandSucceeds(candidate + " -c \"import PIL\"");
        }

        // Score by capability: python=1, pip=2, pillow=4 (pillow is most important)
        int score = 0;
        if (candidate_status.python_found) score += 1;
        if (candidate_status.pip_found) score += 2;
        if (candidate_status.pillow_found) score += 4;

        if (score > best_score) {
            best_status = candidate_status;
            best_score = score;
        }
    }

    return best_status;
}

std::string QuoteArg(const std::string& value) {
    std::string escaped;
    for (char c : value) {
        if (c == '"') escaped += '\\';  // Escape embedded quotes
        escaped += c;
    }
    return '"' + escaped + '"';
}

int RunAtlasBakeCommand(const std::string& python_invocation,
                        const fs::path& script_path,
                        const fs::path& output_path,
                        const std::string& home_sections) {
    std::error_code ec;
    fs::create_directories(output_path.parent_path(), ec);
    if (ec) return -1;

    const std::string script = QuoteArg(script_path.string());
    const std::string output = QuoteArg(output_path.string());
    const std::string sections = QuoteArg(home_sections);
    const std::string command =
        python_invocation + " " + script + " --output " + output + " --home-sections " + sections;

    return std::system(command.c_str());
}

int RunPillowInstallCommand(const std::string& python_invocation) {
    const std::string command = python_invocation + " -m pip install --upgrade Pillow";
    return std::system(command.c_str());
}

int RunEnsurePipCommand(const std::string& python_invocation) {
    const std::string command = python_invocation + " -m ensurepip --upgrade";
    return std::system(command.c_str());
}

void OpenUriInBrowser(const std::string& uri) {
    if (uri.empty()) return;

#ifdef _WIN32
    // Use ShellExecuteW to avoid shell injection and console window flash
    std::wstring wide_uri(uri.begin(), uri.end());
    ShellExecuteW(nullptr, L"open", wide_uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif __APPLE__
    std::string command = "open " + QuoteArg(uri);
    std::system(command.c_str());
#else
    std::string command = "xdg-open " + QuoteArg(uri) + " &";
    std::system(command.c_str());
#endif
}

void OpenFolderInFileBrowser(const fs::path& path) {
    std::error_code ec;
    fs::path dir = fs::is_directory(path, ec) ? path : path.parent_path();
    if (dir.empty() || ec) return;

#ifdef _WIN32
    // Use ShellExecuteW to avoid a wierd console window flash
    std::wstring wide_dir = dir.wstring();
    ShellExecuteW(nullptr, L"explore", wide_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif __APPLE__
    std::string command = "open " + QuoteArg(dir.string());
    std::system(command.c_str());
#else
    std::string command = "xdg-open " + QuoteArg(dir.string()) + " &";
    std::system(command.c_str());
#endif
}

} // namespace

namespace YipOS {

void UIManager::RenderDisplayTab(PDAController& pda, Config& config) {
    ImGui::Text("Display & Timing");
    ImGui::TextDisabled("Calibrate the Williams Tube write head position and timing.");

    ImGui::Separator();

    ImGui::Text("Y Calibration");
    ImGui::TextDisabled("Adjusts vertical positioning of text on the CRT display.");
    ImGui::SliderFloat("Y Offset", &config.y_offset, -0.5f, 0.5f);
    ImGui::SameLine(); if (ImGui::SmallButton("Reset##yoff")) config.y_offset = 0.0f;
    ImGui::SliderFloat("Y Scale", &config.y_scale, 0.1f, 2.0f);
    ImGui::SameLine(); if (ImGui::SmallButton("Reset##yscl")) config.y_scale = 1.0f;
    ImGui::SliderFloat("Y Curve", &config.y_curve, 0.1f, 3.0f);
    ImGui::SameLine(); if (ImGui::SmallButton("Reset##ycur")) config.y_curve = 1.0f;

    ImGui::Separator();

    ImGui::Text("Write Timing");
    ImGui::TextDisabled("Controls how fast characters are written to the display.");
    ImGui::SliderFloat("Write Delay", &config.write_delay, 0.01f, 0.2f, "%.3f s");
    ImGui::SliderFloat("Settle Delay", &config.settle_delay, 0.01f, 0.1f, "%.3f s");
    ImGui::SliderFloat("Refresh Interval", &config.refresh_interval, 0.0f, 30.0f, "%.1f s");
    ImGui::TextDisabled("How often the full screen is re-rendered (0 = never).");

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Home Menu", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Statics persist across frames for bake status and environment detection
        static std::string home_bake_status;
        static bool home_bake_status_error = false;
        static BakeEnvironmentStatus bake_environment;
        static bool bake_environment_checked = false;

        auto refresh_bake_environment = [&]() {
            bake_environment = DetectBakeEnvironment(assets_path_);
            bake_environment_checked = true;
        };

        if (!bake_environment_checked) {
            refresh_bake_environment();
        }

        const auto section_labels = Glyphs::GetDefaultHomeSectionLabels();
        const auto visible_sections = GetVisibleHomeSections(config);
        const std::string current_sections = SerializeHomeSections(visible_sections);
        const auto baked_sections = YipOS::Glyphs::ParseHomeSectionLabels(config.GetState("home.baked.labels"));
        const std::string baked_sections_serialized = SerializeHomeSections(baked_sections);
        const bool has_baked_sections = !baked_sections.empty();
        const bool baked_matches_current = has_baked_sections && current_sections == baked_sections_serialized;
        const bool can_bake = bake_environment.script_found && bake_environment.python_found && bake_environment.pip_found && bake_environment.pillow_found;
        std::string baked_output = config.GetState("home.baked.output");

        bool home_sections_changed = false;
        bool rerender_home = false;
        bool baked_enabled = config.GetState("home.baked.enabled", "0") == "1";

        ImGui::TextDisabled("Choose which sections appear on the PDA home menu.");
        ImGui::TextWrapped("If you change the selection below, rebake the atlas and re-import the PNG before expecting the external asset to update.");

        if (!has_baked_sections) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                               "No baked atlas yet. Pick your menu items, then bake a new PNG.");
        } else if (!baked_matches_current) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                               "Selection changed since the last bake. Rebake required for the external asset.");
        } else if (baked_enabled) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f),
                               "Baked atlas is in sync and currently enabled.");
        } else {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f),
                               "Current selection matches the baked atlas.");
        }

        if (ImGui::Checkbox("Use baked home atlas", &baked_enabled)) {
            config.SetState("home.baked.enabled", baked_enabled ? "1" : "0");
            rerender_home = true;
            if (!config_path_.empty()) config.SaveToFile(config_path_);
        }
        ImGui::TextDisabled("Use this only after the baked PNG has been imported into the external asset.");

        ImGui::BeginDisabled(!can_bake);
        if (ImGui::Button("Bake Home Atlas")) {
            fs::path script_path = GetMacroAtlasScriptPath(assets_path_);
            fs::path output_path = GetBakedAtlasOutputPath(config_path_);

            int result = RunAtlasBakeCommand(bake_environment.python_invocation,
                                             script_path,
                                             output_path,
                                             current_sections);
            if (result == 0) {
                config.SetState("home.baked.labels", current_sections);
                config.SetState("home.baked.output", output_path.string());
                if (!config_path_.empty()) config.SaveToFile(config_path_);
                home_bake_status = "Baked atlas exported to " + output_path.string() +
                                   ". Import it externally, then enable baked home atlas.";
                home_bake_status_error = false;
            } else {
                home_bake_status = "Atlas bake failed even though Python + Pillow were detected. Check the console output and try rebaking again.";
                home_bake_status_error = true;
            }
        }
        ImGui::EndDisabled();

        if (!can_bake && ImGui::IsItemHovered()) {
            if (!bake_environment.script_found) {
                ImGui::SetTooltip("The source bake script is not available in this build.");
            } else if (!bake_environment.python_found) {
                ImGui::SetTooltip("Install Python 3 to enable atlas baking.");
            } else if (!bake_environment.pillow_found) {
                ImGui::SetTooltip("Install Pillow in the detected Python environment to enable atlas baking.");
            }
        }

        if (!baked_output.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Open Bake Folder")) {
                OpenFolderInFileBrowser(fs::path(baked_output));
            }
        }

        if (!home_bake_status.empty()) {
            ImGui::TextColored(home_bake_status_error
                                   ? ImVec4(1.0f, 0.4f, 0.3f, 1.0f)
                                   : ImVec4(0.2f, 1.0f, 0.4f, 1.0f),
                               "%s", home_bake_status.c_str());
        }

        if (!baked_output.empty()) {
            ImGui::TextWrapped("Last baked atlas: %s", baked_output.c_str());
        }

        ImGui::Separator();
        ImGui::Text("Menu Items");
        ImGui::TextDisabled("These pack left-to-right into the baked home menu pages.");

        if (ImGui::Button("Show All##home_sections")) {
            for (const auto& label : section_labels) {
                config.SetState(Glyphs::GetHomeSectionStateKey(label), "1");
            }
            home_sections_changed = true;
            rerender_home = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Hide All##home_sections")) {
            for (const auto& label : section_labels) {
                config.SetState(Glyphs::GetHomeSectionStateKey(label), "0");
            }
            home_sections_changed = true;
            rerender_home = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d / %d selected", static_cast<int>(visible_sections.size()), static_cast<int>(section_labels.size()));

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 6.0f));
        ImGui::Columns(5, "##home_sections_grid", false);
        for (const auto& label : section_labels) {
            bool visible = config.GetState(Glyphs::GetHomeSectionStateKey(label), "1") != "0";
            std::string checkbox_id = label + "##home_section";
            if (ImGui::Checkbox(checkbox_id.c_str(), &visible)) {
                config.SetState(Glyphs::GetHomeSectionStateKey(label), visible ? "1" : "0");
                home_sections_changed = true;
                rerender_home = true;
            }
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
        ImGui::PopStyleVar();

        ImGui::Separator();
        ImGui::Text("Bake Prerequisites");
        ImGui::TextColored(bake_environment.script_found
                               ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f)
                               : ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                           bake_environment.script_found
                               ? "Source script found"
                               : "Source script missing");
        ImGui::TextColored(bake_environment.python_found
                               ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f)
                               : ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                           bake_environment.python_found
                               ? "Python detected"
                               : "Python 3 not detected");
        ImGui::TextColored((!bake_environment.python_found || bake_environment.pip_found)
                               ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f)
                               : ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                           (!bake_environment.python_found || bake_environment.pip_found)
                               ? "pip available"
                               : "pip missing");
        ImGui::TextColored(bake_environment.pillow_found
                               ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f)
                               : ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                           bake_environment.pillow_found
                               ? "Pillow detected"
                               : "Pillow missing");
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh Status##home_bake_env")) {
            refresh_bake_environment();
        }

        if (!bake_environment.script_found) {
            ImGui::TextDisabled("Home atlas baking is not available in this build.");
        } else if (!bake_environment.python_found) {
            ImGui::TextWrapped("To enable atlas baking, install Python 3 first.");
#ifdef _WIN32
            ImGui::BulletText("Windows easiest: install Python from python.org and enable the Python Launcher or Add python.exe to PATH.");
            ImGui::BulletText("After that, install Pillow with: py -3 -m pip install pillow");
#else
            ImGui::BulletText("Install Python 3 and make sure python3 is available on PATH.");
            ImGui::BulletText("Then install Pillow with: python3 -m pip install pillow");
#endif
            if (ImGui::Button("Open Python Download")) {
#ifdef _WIN32
                OpenUriInBrowser("https://www.python.org/downloads/windows/");
#else
                OpenUriInBrowser("https://www.python.org/downloads/");
#endif
            }
        } else if (!bake_environment.pip_found) {
            ImGui::TextWrapped("Python is installed, but pip is missing or not ready in that environment.");
            ImGui::BulletText("Try repairing pip with: %s -m ensurepip --upgrade", bake_environment.python_invocation.c_str());
            if (ImGui::Button("Install / Repair pip")) {
                int result = RunEnsurePipCommand(bake_environment.python_invocation);
                refresh_bake_environment();
                if (result == 0 && bake_environment.pip_found) {
                    home_bake_status = "pip is now available. You can install Pillow from here.";
                    home_bake_status_error = false;
                } else {
                    home_bake_status = "Could not repair pip automatically. Try the command shown above in a terminal.";
                    home_bake_status_error = true;
                }
            }
        } else {
            ImGui::TextDisabled("Atlas baking tools are ready.");
        }

        if (bake_environment.script_found && bake_environment.python_found && bake_environment.pip_found) {
            if (!bake_environment.pillow_found) {
                ImGui::TextWrapped("Python is installed, but the Pillow package is missing for this feature.");
                ImGui::BulletText("Install Pillow with: %s -m pip install pillow", bake_environment.python_invocation.c_str());
            }

            if (ImGui::Button(bake_environment.pillow_found ? "Repair Pillow" : "Install Pillow")) {
                int result = RunPillowInstallCommand(bake_environment.python_invocation);
                refresh_bake_environment();
                if (result == 0 && bake_environment.pillow_found) {
                    home_bake_status = "Pillow is installed and atlas baking is ready to use.";
                    home_bake_status_error = false;
                } else {
                    home_bake_status = "Pillow install failed. Check the console output and try again, or run the command shown above.";
                    home_bake_status_error = true;
                }
            }
        }

        if (home_sections_changed) {
            if (!config_path_.empty()) config.SaveToFile(config_path_);
        }

        if (rerender_home) {
            if (Screen* current = pda.GetCurrentScreen(); current && current->name == "HOME") {
                pda.StartRender(current);
            }
        }


        ImGui::Separator();
    }

    static const char* levels[] = {"DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"};
    static int current_level = 1;
    if (ImGui::Combo("Log Level", &current_level, levels, 5)) {
        config.log_level = levels[current_level];
        Logger::SetLogLevel(Logger::StringToLevel(config.log_level));
    }

    ImGui::Separator();

    if (ImGui::Button("Save")) {
        if (!config_path_.empty()) config.SaveToFile(config_path_);
    }
    ImGui::SameLine();
    if (!pda.IsBooting()) {
        if (ImGui::Button("Reboot PDA")) {
            pda.Reboot();
        }
    }
}

} // namespace YipOS

