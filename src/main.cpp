#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <algorithm>
#include <random>
#include <vector>

using namespace geode::prelude;

// Xáo trộn vị trí của một nhóm nút (các nút đổi chỗ cho nhau).
static void shuffleGroup(std::vector<CCMenuItem*>& items) {
    if (items.size() < 2) return;

    // Lưu vị trí theo toạ độ thế giới vì các nút nằm trong những menu khác nhau.
    std::vector<CCPoint> positions;
    positions.reserve(items.size());
    for (auto item : items) {
        positions.push_back(item->getParent()->convertToWorldSpace(item->getPosition()));
    }

    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(positions.begin(), positions.end(), rng);

    for (size_t i = 0; i < items.size(); i++) {
        auto item = items[i];
        item->setPosition(item->getParent()->convertToNodeSpace(positions[i]));
    }
}

class $modify(ShuffleMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        // Chạy ở frame kế tiếp để các mod khác (và layout của game) thêm nút xong trước.
        Ref<MenuLayer> self = this;
        Loader::get()->queueInMainThread([self]() {
            ShuffleMenuLayer::shuffleButtons(self);
        });

        return true;
    }

    static void shuffleButtons(MenuLayer* layer) {
        bool withinMenu = Mod::get()->getSettingValue<bool>("within-menu");

        std::vector<CCMenuItem*> all;

        auto children = layer->getChildren();
        if (!children) return;

        for (auto child : CCArrayExt<CCNode*>(children)) {
            auto menu = typeinfo_cast<CCMenu*>(child);
            if (!menu || !menu->getChildren()) continue;

            std::vector<CCMenuItem*> group;
            for (auto node : CCArrayExt<CCNode*>(menu->getChildren())) {
                auto item = typeinfo_cast<CCMenuItem*>(node);
                if (item && item->isVisible()) group.push_back(item);
            }

            if (withinMenu) {
                shuffleGroup(group);
            } else {
                all.insert(all.end(), group.begin(), group.end());
            }
        }

        if (!withinMenu) shuffleGroup(all);
    }
};
