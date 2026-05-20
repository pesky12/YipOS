#include "HomeScreen.hpp"
#include "app/PDAController.hpp"
#include "app/PDADisplay.hpp"
#include "core/Config.hpp"
#include "core/Logger.hpp"
#include <cstdio>

namespace YipOS {

using namespace Glyphs;

namespace {

const std::string* GetPageLabel(const std::vector<std::string>& labels, int page, int tx, int ty) {
    size_t index = static_cast<size_t>(page) * HOME_PAGE_SLOT_COUNT +
                   static_cast<size_t>(ty) * TILE_COLS +
                   static_cast<size_t>(tx);
    if (index >= labels.size()) return nullptr;
    return &labels[index];
}

} // namespace

HomeScreen::HomeScreen(PDAController& pda) : Screen(pda) {
    name = "HOME";
    for (auto& row : tile_highlighted_) row.fill(false);
    SyncLayoutForRender();
}

void HomeScreen::SyncLayoutForRender() {
    RefreshVisibleTiles();
}

void HomeScreen::RefreshVisibleTiles() {
    std::vector<std::string> next_visible;
    auto& config = pda_.GetConfig();

    // Use baked labels if enabled, otherwise use default visible sections
    if (config.GetState("home.baked.enabled", "0") == "1") {
        auto baked = ParseHomeSectionLabels(config.GetState("home.baked.labels"));
        if (!baked.empty()) {
            next_visible = baked;
        } else {
            // Fallback to default if baked labels are empty
            auto labels = GetDefaultHomeSectionLabels();
            for (const auto& label : labels) {
                if (config.GetState(GetHomeSectionStateKey(label), "1") != "0") {
                    next_visible.push_back(label);
                }
            }
        }
    } else {
        auto labels = GetDefaultHomeSectionLabels();
        for (const auto& label : labels) {
            if (config.GetState(GetHomeSectionStateKey(label), "1") != "0") {
                next_visible.push_back(label);
            }
        }
    }

    visible_labels_ = std::move(next_visible);

    int max_page = std::max(0, GetPageCount() - 1);
    if (page_ > max_page) {
        page_ = max_page;
    }

    macro_index = (page_ == 0) ? 0 : 30;
}

const std::string* HomeScreen::GetVisibleLabel(int tx, int ty) const {
    return GetPageLabel(visible_labels_, page_, tx, ty);
}

int HomeScreen::GetPageCount() const {
    if (visible_labels_.empty()) return 1;
    return static_cast<int>((visible_labels_.size() + HOME_PAGE_SLOT_COUNT - 1) / HOME_PAGE_SLOT_COUNT);
}

void HomeScreen::Render() {
    RenderFrame("YIP OS");
    RenderContent();
    RenderPageIndicators();
    RenderStatusBar();
    Logger::Debug("Home screen rendered");
}

void HomeScreen::RenderContent() {
    for (int ty = 0; ty < TILE_ROWS; ty++) {
        int row = ZONE_ROWS[ty];
        display_.WriteGlyph(0, row, G_VLINE);
        for (int tx = 0; tx < TILE_COLS; tx++) {
            WriteTile(tx, ty);
        }
        display_.WriteGlyph(COLS - 1, row, G_VLINE);
    }
}

void HomeScreen::RenderDynamic() {
    // Notification indicators — page-specific
    if (page_ == 0) {
        // VRCX tile (1,0): show "*" indicator when unseen notifications exist
        if (pda_.HasUnseenNotifs()) {
            display_.WriteChar(6, ZONE_ROWS[1], static_cast<int>('*') + INVERT_OFFSET);
        } else {
            display_.WriteChar(6, ZONE_ROWS[1], static_cast<int>(' '));
        }

        // CHAT tile (1,4): show "*" indicator when unseen chat messages exist
        if (pda_.HasUnseenChatCached()) {
            display_.WriteChar(38, ZONE_ROWS[1], static_cast<int>('*') + INVERT_OFFSET);
        } else {
            display_.WriteChar(38, ZONE_ROWS[1], static_cast<int>(' '));
        }
    } else if (page_ == 1) {
        // DM tile (0,4): show "!" indicator when unseen DMs exist
        if (pda_.HasUnseenDMCached()) {
            display_.WriteChar(38, ZONE_ROWS[0], static_cast<int>('!') + INVERT_OFFSET);
        } else {
            display_.WriteChar(38, ZONE_ROWS[0], static_cast<int>(' '));
        }
        // FUR tile (1,0): show "*" indicator when a marked event is in window
        if (pda_.HasFurEventWindow()) {
            display_.WriteChar(6, ZONE_ROWS[1], static_cast<int>('*') + INVERT_OFFSET);
        } else {
            display_.WriteChar(6, ZONE_ROWS[1], static_cast<int>(' '));
        }
    }

    RenderPageIndicators();
    RenderClock();
    RenderCursor();
}

void HomeScreen::RenderPageIndicators() {
    int page_count = GetPageCount();

    char pos[8];
    std::snprintf(pos, sizeof(pos), "%d/%d", page_ + 1, page_count);
    display_.WriteText(5, 7, pos);

    display_.WriteGlyph(0, 3, page_ > 0 ? G_UP : G_VLINE);
    display_.WriteGlyph(0, 5, page_ < page_count - 1 ? G_DOWN : G_VLINE);
}

void HomeScreen::WriteTile(int tx, int ty) {
    // Use visible labels when baked atlas is enabled, otherwise use TILE_LABELS
    const std::string* dynamic_label = GetVisibleLabel(tx, ty);
    const char* static_label = TILE_LABELS[page_][ty][tx].text;
    
    bool is_active = dynamic_label != nullptr;
    bool inverted = is_active && !tile_highlighted_[ty][tx];
    
    // Use dynamic label for display when available
    std::string label_str;
    if (dynamic_label) {
        label_str = *dynamic_label;
        // VRCX label gets "*" suffix when unseen notifications exist (page 0 only)
        if (*dynamic_label == "VRCX" && page_ == 0 && pda_.HasUnseenNotifs()) {
            label_str += "*";
        }
    } else {
        label_str = static_label;
        // VRCX tile (1,0) gets "*" suffix when unseen notifications exist (page 0 only)
        if (page_ == 0 && ty == 1 && tx == 0 && pda_.HasUnseenNotifs()) {
            label_str = std::string(static_label) + "*";
        }
    }

    int len = static_cast<int>(label_str.size());
    int center = TILE_CENTERS[tx];
    int start_col = center - len / 2;
    int row = ZONE_ROWS[ty];

    for (int i = 0; i < len; i++) {
        int c = start_col + i;
        if (c < 1 || c >= COLS - 1) continue;
        char ch = label_str[i];
        int char_idx = (ch >= 32 && ch <= 126) ? static_cast<int>(ch) : 32;
        if (inverted) char_idx += INVERT_OFFSET;
        display_.WriteChar(c, row, char_idx);
    }
}

bool HomeScreen::OnInput(const std::string& key) {
    SyncLayoutForRender();

    // ML = page up
    if (key == "ML" && page_ > 0) {
        page_--;
        macro_index = (page_ == 0) ? 0 : 30;
        for (auto& row : tile_highlighted_) row.fill(false);
        pda_.StartRender(this);
        return true;
    }
    // BL = page down
    if (key == "BL" && page_ < GetPageCount() - 1) {
        page_++;
        macro_index = (page_ == 0) ? 0 : 30;
        for (auto& row : tile_highlighted_) row.fill(false);
        pda_.StartRender(this);
        return true;
    }

    if (key.size() == 2 && key[0] >= '1' && key[0] <= '5' && key[1] >= '1' && key[1] <= '3') {
        int tx = key[0] - '1';
        int ty = key[1] - '1';

        if (tx >= 0 && tx < TILE_COLS && ty >= 0 && ty < TILE_ROWS) {
            const std::string* label = GetVisibleLabel(tx, ty);
            if (!label) {
                Logger::Debug("Tile (" + std::to_string(tx) + "," + std::to_string(ty) + ") is empty");
                return true;
            }

            // Highlight tile — force immediate writes for flash visibility
            display_.CancelBuffered();
            tile_highlighted_[ty][tx] = true;
            WriteTile(tx, ty);
            Logger::Info("Tile (" + std::to_string(tx) + "," + std::to_string(ty) +
                        ") '" + *label + "' -> navigating");

            pda_.SetPendingNavigate(*label);
            return true;
        }
    }
    return false;
}

void HomeScreen::Update() {}

} // namespace YipOS
