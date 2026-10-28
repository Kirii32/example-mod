#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace geode::prelude;

namespace {
    std::mt19937& rng() {
        static std::mt19937 gen{std::random_device{}()};
        return gen;
    }

    float randRange(float a, float b) {
        return std::uniform_real_distribution<float>(a, b)(rng());
    }

    // Nửa kích thước của nút tính theo toạ độ màn hình (đã nhân scale của menu cha).
    CCSize worldHalfSize(CCMenuItem* item) {
        auto parent = item->getParent();
        auto p0 = parent->convertToWorldSpace(ccp(0, 0));
        auto px = parent->convertToWorldSpace(ccp(1, 0));
        auto py = parent->convertToWorldSpace(ccp(0, 1));
        float sx = std::abs(px.x - p0.x);
        float sy = std::abs(py.y - p0.y);
        auto size = item->getScaledContentSize();
        return CCSize(size.width * sx / 2.f, size.height * sy / 2.f);
    }

    CCPoint toWorld(CCMenuItem* item) {
        return item->getParent()->convertToWorldSpace(item->getPosition());
    }

    void setWorldPos(CCMenuItem* item, CCPoint world) {
        item->setPosition(item->getParent()->convertToNodeSpace(world));
    }
}

struct Bouncer {
    Ref<CCMenuItem> item;
    CCPoint vel;
};

// Node phụ chạy update mỗi frame để làm các nút nảy như màn hình chờ DVD.
class BounceNode : public CCNode {
protected:
    std::vector<Bouncer> m_items;

    bool init() override {
        if (!CCNode::init()) return false;
        this->scheduleUpdate();
        return true;
    }

public:
    static BounceNode* create() {
        auto ret = new BounceNode();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void collect(CCNode* layer) {
        float speed = static_cast<float>(Mod::get()->getSettingValue<double>("speed"));
        bool randomStart = Mod::get()->getSettingValue<bool>("random-start");
        auto win = CCDirector::get()->getWinSize();

        auto children = layer->getChildren();
        if (!children) return;

        for (auto child : CCArrayExt<CCNode*>(children)) {
            auto menu = typeinfo_cast<CCMenu*>(child);
            if (!menu || !menu->getChildren()) continue;

            for (auto node : CCArrayExt<CCNode*>(menu->getChildren())) {
                auto item = typeinfo_cast<CCMenuItem*>(node);
                if (!item || !item->isVisible()) continue;

                if (randomStart) {
                    // Đặt nút ở một vị trí ngẫu nhiên bất kỳ trên màn hình.
                    auto half = worldHalfSize(item);
                    float x = (win.width > half.width * 2.f)
                        ? randRange(half.width, win.width - half.width)
                        : win.width / 2.f;
                    float y = (win.height > half.height * 2.f)
                        ? randRange(half.height, win.height - half.height)
                        : win.height / 2.f;
                    setWorldPos(item, ccp(x, y));
                }

                // Hướng bay chéo ngẫu nhiên (tránh bay gần như song song với trục).
                float angle = randRange(0.35f, 1.2f);
                float dirX = (randRange(0.f, 1.f) < 0.5f) ? -1.f : 1.f;
                float dirY = (randRange(0.f, 1.f) < 0.5f) ? -1.f : 1.f;
                CCPoint vel = ccp(std::cos(angle) * speed * dirX, std::sin(angle) * speed * dirY);

                m_items.push_back({ Ref<CCMenuItem>(item), vel });
            }
        }
    }

    void update(float dt) override {
        dt = std::min(dt, 0.1f); // tránh nút "teleport" khi game bị đứng hình
        auto win = CCDirector::get()->getWinSize();

        for (auto& b : m_items) {
            auto item = b.item.data();
            if (!item || !item->getParent()) continue;

            auto pos = toWorld(item);
            auto half = worldHalfSize(item);

            pos.x += b.vel.x * dt;
            pos.y += b.vel.y * dt;

            if (pos.x - half.width < 0.f) {
                pos.x = half.width;
                b.vel.x = std::abs(b.vel.x);
            } else if (pos.x + half.width > win.width) {
                pos.x = win.width - half.width;
                b.vel.x = -std::abs(b.vel.x);
            }

            if (pos.y - half.height < 0.f) {
                pos.y = half.height;
                b.vel.y = std::abs(b.vel.y);
            } else if (pos.y + half.height > win.height) {
                pos.y = win.height - half.height;
                b.vel.y = -std::abs(b.vel.y);
            }

            setWorldPos(item, pos);
        }
    }
};

class $modify(BounceMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        // Chạy ở frame kế tiếp để các mod khác thêm nút xong trước.
        Ref<MenuLayer> self = this;
        Loader::get()->queueInMainThread([self]() {
            auto node = BounceNode::create();
            if (!node) return;
            node->collect(self);
            self->addChild(node);
        });

        return true;
    }
};
