#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <functional>
#include <string>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Dữ liệu lưu giữa các lần mở game
// ---------------------------------------------------------------------------
namespace qs {
    int targetId() { return Mod::get()->getSavedValue<int>("target-level-id", 0); }
    std::string targetName() { return Mod::get()->getSavedValue<std::string>("target-level-name", ""); }
    float holdTime() { return static_cast<float>(Mod::get()->getSavedValue<double>("hold-time", 0.5)); }
    bool warn() { return Mod::get()->getSavedValue<bool>("warn-switch", false); }

    void setTarget(int id, std::string const& name) {
        Mod::get()->setSavedValue<int>("target-level-id", id);
        Mod::get()->setSavedValue<std::string>("target-level-name", name);
    }
    void clearTarget() { setTarget(0, ""); }
    void setHoldTime(float t) { Mod::get()->setSavedValue<double>("hold-time", t); }
    void setWarn(bool b) { Mod::get()->setSavedValue<bool>("warn-switch", b); }

    // Lấy sprite từ frame có sẵn trong game, thử lần lượt từng tên.
    CCSprite* frameSprite(std::initializer_list<char const*> names) {
        auto cache = CCSpriteFrameCache::get();
        for (auto name : names) {
            if (cache->spriteFrameByName(name)) {
                return CCSprite::createWithSpriteFrameName(name);
            }
        }
        return CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png");
    }
}

// ---------------------------------------------------------------------------
// Tải level theo ID (dùng level đã lưu nếu có, nếu chưa thì tải từ server)
// ---------------------------------------------------------------------------
class LevelFetcher : public CCObject, public LevelDownloadDelegate {
protected:
    int m_id = 0;
    std::function<void(GJGameLevel*)> m_onDone;
    std::function<void()> m_onFail;
    LevelDownloadDelegate* m_prev = nullptr;

    void restoreDelegate() {
        auto glm = GameLevelManager::get();
        if (glm->m_levelDownloadDelegate == this) {
            glm->m_levelDownloadDelegate = m_prev;
        }
    }

public:
    // Trả về true nếu phải tải qua mạng (bất đồng bộ), false nếu có sẵn và gọi callback ngay.
    static bool fetch(int id, std::function<void(GJGameLevel*)> onDone, std::function<void()> onFail) {
        auto glm = GameLevelManager::get();
        if (auto level = glm->getSavedLevel(id); level && !level->m_levelString.empty()) {
            onDone(level);
            return false;
        }

        auto f = new LevelFetcher(); // refcount = 1, tự release khi xong
        f->m_id = id;
        f->m_onDone = std::move(onDone);
        f->m_onFail = std::move(onFail);
        f->m_prev = glm->m_levelDownloadDelegate;
        glm->m_levelDownloadDelegate = f;
        glm->downloadLevel(id, false, 0);
        return true;
    }

    void levelDownloadFinished(GJGameLevel* level) override {
        if (!level || level->m_levelID.value() != m_id) return; // không phải level mình đang chờ
        auto cb = m_onDone;
        this->restoreDelegate();
        this->release();
        if (cb) cb(level);
    }

    void levelDownloadFailed(int) override {
        auto cb = m_onFail;
        this->restoreDelegate();
        this->release();
        if (cb) cb();
    }
};

// ---------------------------------------------------------------------------
// Chuyển sang level đã chọn
// ---------------------------------------------------------------------------
namespace qs {
    void startLevel(GJGameLevel* level) {
        auto scene = PlayLayer::scene(level, false, false);
        CCDirector::get()->replaceScene(CCTransitionFade::create(0.5f, scene));
    }

    void switchToTarget() {
        int id = targetId();
        if (id <= 0) {
            Notification::create("No level selected. Use the + button in the level page.", NotificationIcon::Warning)->show();
            return;
        }
        bool async = LevelFetcher::fetch(
            id,
            [](GJGameLevel* level) { startLevel(level); },
            []() { Notification::create("Could not download the level", NotificationIcon::Error)->show(); }
        );
        if (async) {
            Notification::create("Downloading level...", NotificationIcon::Loading)->show();
        }
    }

    void requestSwitch() {
        if (warn()) {
            createQuickPopup(
                "Switch level",
                "Leave this level and switch to the selected level?\nUnsaved progress will be lost.",
                "Cancel", "Switch",
                [](FLAlertLayer*, bool btn2) {
                    if (btn2) switchToTarget();
                }
            );
        } else {
            switchToTarget();
        }
    }
}

// ---------------------------------------------------------------------------
// Nút phải giữ một khoảng thời gian mới kích hoạt (có vòng tiến trình)
// ---------------------------------------------------------------------------
class HoldButton : public CCMenuItem {
protected:
    std::function<void()> m_onHold;
    CCProgressTimer* m_ring = nullptr;
    float m_holdTime = 0.5f;
    float m_elapsed = 0.f;
    float m_baseScale = 1.f;
    bool m_holding = false;

    bool init(CCSprite* icon, CCSprite* ringSprite, float holdTime, std::function<void()> onHold, float scale) {
        if (!CCMenuItem::init()) return false;

        m_holdTime = std::max(holdTime, 0.05f);
        m_onHold = std::move(onHold);
        m_baseScale = scale;

        auto size = icon->getContentSize();
        this->setContentSize(size);
        icon->setPosition(ccp(size.width / 2.f, size.height / 2.f));
        this->addChild(icon);

        ringSprite->setOpacity(170);
        m_ring = CCProgressTimer::create(ringSprite);
        m_ring->setType(kCCProgressTimerTypeRadial);
        m_ring->setPercentage(0.f);
        m_ring->setPosition(ccp(size.width / 2.f, size.height / 2.f));
        this->addChild(m_ring, 1);

        this->setScale(scale);
        return true;
    }

    void resetHold() {
        m_holding = false;
        m_elapsed = 0.f;
        this->unscheduleUpdate();
        m_ring->setPercentage(0.f);
        this->stopAllActions();
        this->runAction(CCScaleTo::create(0.1f, m_baseScale));
    }

public:
    static HoldButton* create(CCSprite* icon, CCSprite* ring, float holdTime, std::function<void()> onHold, float scale = 1.f) {
        auto ret = new HoldButton();
        if (ret->init(icon, ring, holdTime, std::move(onHold), scale)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void selected() override {
        CCMenuItem::selected();
        m_holding = true;
        m_elapsed = 0.f;
        this->stopAllActions();
        this->runAction(CCScaleTo::create(0.1f, m_baseScale * 1.15f));
        this->scheduleUpdate();
    }

    void unselected() override {
        CCMenuItem::unselected();
        this->resetHold();
    }

    // Nhả tay sớm thì không làm gì cả.
    void activate() override {}

    void update(float dt) override {
        if (!m_holding) return;
        m_elapsed += dt;
        m_ring->setPercentage(std::min(100.f, m_elapsed / m_holdTime * 100.f));
        if (m_elapsed >= m_holdTime) {
            m_holding = false;
            this->unscheduleUpdate();
            auto cb = m_onHold;
            if (cb) cb(); // có thể thay scene, nên không đụng gì thêm sau dòng này
        }
    }
};

// ---------------------------------------------------------------------------
// Menu cài đặt: nhập ID level, thời gian giữ, cảnh báo
// ---------------------------------------------------------------------------
class QuickSwitchPopup : public Popup {
protected:
    TextInput* m_idInput = nullptr;
    TextInput* m_holdInput = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemToggler* m_warnToggle = nullptr;
    CCMenuItemSpriteExtra* m_okBtn = nullptr;
    bool m_busy = false;

    bool init() {
        if (!Popup::init(390.f, 230.f)) return false;
        this->setTitle("Quick Level Switch");

        // Hàng 1: ô nhập ID, nút xoá, nút OK
        m_idInput = TextInput::create(170.f, "Enter level ID");
        m_idInput->setCommonFilter(CommonFilter::Uint);
        m_idInput->setMaxCharCount(10);
        if (int id = qs::targetId(); id > 0) {
            m_idInput->setString(std::to_string(id));
        }
        m_mainLayer->addChildAtPosition(m_idInput, Anchor::Center, ccp(-45.f, 50.f));

        auto trashSpr = CCSprite::createWithSpriteFrameName("GJ_trashBtn_001.png");
        trashSpr->setScale(0.8f);
        auto trashBtn = CCMenuItemSpriteExtra::create(trashSpr, this, menu_selector(QuickSwitchPopup::onClear));
        m_buttonMenu->addChildAtPosition(trashBtn, Anchor::Center, ccp(65.f, 50.f));

        auto okSpr = ButtonSprite::create("OK");
        m_okBtn = CCMenuItemSpriteExtra::create(okSpr, this, menu_selector(QuickSwitchPopup::onOk));
        m_buttonMenu->addChildAtPosition(m_okBtn, Anchor::Center, ccp(135.f, 50.f));

        // Hàng 2: level đang được chọn
        m_status = CCLabelBMFont::create("", "chatFont.fnt");
        m_status->setScale(0.7f);
        m_mainLayer->addChildAtPosition(m_status, Anchor::Center, ccp(0.f, 10.f));
        this->refreshStatus();

        // Hàng 3: thời gian giữ
        auto holdLabel = CCLabelBMFont::create("Hold time (seconds):", "chatFont.fnt");
        holdLabel->setScale(0.7f);
        holdLabel->setAnchorPoint(ccp(0.f, 0.5f));
        m_mainLayer->addChildAtPosition(holdLabel, Anchor::Center, ccp(-170.f, -30.f));

        m_holdInput = TextInput::create(60.f, "0.5");
        m_holdInput->setCommonFilter(CommonFilter::Float);
        m_holdInput->setMaxCharCount(5);
        m_holdInput->setString(fmt::format("{:.2f}", qs::holdTime()));
        m_mainLayer->addChildAtPosition(m_holdInput, Anchor::Center, ccp(-10.f, -30.f));

        // Hàng 4: checkbox cảnh báo
        m_warnToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(QuickSwitchPopup::onToggleWarn), 0.7f
        );
        m_warnToggle->toggle(qs::warn());
        m_buttonMenu->addChildAtPosition(m_warnToggle, Anchor::Center, ccp(-165.f, -70.f));

        auto warnLabel = CCLabelBMFont::create("Warn before switching levels", "chatFont.fnt");
        warnLabel->setScale(0.7f);
        warnLabel->setAnchorPoint(ccp(0.f, 0.5f));
        m_mainLayer->addChildAtPosition(warnLabel, Anchor::Center, ccp(-145.f, -70.f));

        return true;
    }

    void refreshStatus() {
        int id = qs::targetId();
        std::string text;
        if (id > 0) {
            auto name = qs::targetName();
            if (name.size() > 24) name = name.substr(0, 24) + "...";
            text = fmt::format("Selected: {} ({})", name, id);
        } else {
            text = "No level selected";
        }
        m_status->setString(text.c_str());
    }

    void onToggleWarn(CCObject*) {}

    void onClear(CCObject*) {
        if (m_busy) return;
        m_idInput->setString("");
        qs::clearTarget();
        this->refreshStatus();
    }

    void onOk(CCObject*) {
        if (m_busy) return;

        // Lưu thời gian giữ và cảnh báo
        float t = 0.5f;
        try { t = std::stof(std::string(m_holdInput->getString())); } catch (...) {}
        t = std::clamp(t, 0.1f, 5.f);
        qs::setHoldTime(t);
        qs::setWarn(m_warnToggle->isToggled());

        std::string text = std::string(m_idInput->getString());
        if (text.empty()) {
            qs::clearTarget();
            Notification::create("Settings saved", NotificationIcon::Success)->show();
            this->keyBackClicked();
            return;
        }

        int id = 0;
        try { id = std::stoi(text); } catch (...) {}
        if (id <= 0) {
            Notification::create("Invalid level ID", NotificationIcon::Error)->show();
            return;
        }

        m_busy = true;
        m_okBtn->setEnabled(false);
        m_status->setString("Loading level...");

        Ref<QuickSwitchPopup> self = this;
        LevelFetcher::fetch(
            id,
            [self, id](GJGameLevel* level) { self->onLoaded(id, level); },
            [self]() { self->onFailed(); }
        );
    }

    void onLoaded(int id, GJGameLevel* level) {
        std::string name = std::string(level->m_levelName);
        qs::setTarget(id, name);
        Notification::create(fmt::format("Saved: {}", name), NotificationIcon::Success)->show();
        this->keyBackClicked();
    }

    void onFailed() {
        m_busy = false;
        m_okBtn->setEnabled(true);
        this->refreshStatus();
        Notification::create("Could not download the level", NotificationIcon::Error)->show();
    }

public:
    static QuickSwitchPopup* create() {
        auto ret = new QuickSwitchPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Màn hình sảnh level: thêm nút dấu cộng
// ---------------------------------------------------------------------------
class $modify(QSLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;

        auto winSize = CCDirector::get()->getWinSize();

        auto spr = qs::frameSprite({"GJ_plusBtn_001.png"});
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(QSLevelInfoLayer::onQuickSwitch));
        btn->setID("quick-switch-button"_spr);
        // Vị trí theo tỉ lệ màn hình (cột phải, dưới thông tin level)
        btn->setPosition(ccp(winSize.width * 0.871f, winSize.height * 0.469f));

        auto menu = CCMenu::create();
        menu->setID("quick-switch-menu"_spr);
        menu->setPosition(CCPointZero);
        menu->addChild(btn);
        this->addChild(menu, 10);

        return true;
    }

    void onQuickSwitch(CCObject*) {
        if (auto popup = QuickSwitchPopup::create()) {
            popup->show();
        }
    }
};

// ---------------------------------------------------------------------------
// Menu tạm dừng: nút play phải giữ để chuyển level
// ---------------------------------------------------------------------------
class $modify(QSPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto icon = qs::frameSprite({"GJ_playMusicBtn_001.png", "GJ_playBtn2_001.png"});
        auto ring = qs::frameSprite({"GJ_playMusicBtn_001.png", "GJ_playBtn2_001.png"});
        auto btn = HoldButton::create(icon, ring, qs::holdTime(), []() { qs::requestSwitch(); }, 1.f);
        if (!btn) return;
        btn->setID("quick-switch-hold-button"_spr);

        if (auto menu = this->getChildByID("center-button-menu")) {
            // Chèn vào đầu hàng nút giữa (bên trái nút practice)
            menu->addChild(btn, -100);
            menu->sortAllChildren();
            menu->updateLayout();
        } else {
            // Dự phòng nếu không tìm thấy menu theo ID
            auto winSize = CCDirector::get()->getWinSize();
            btn->setPosition(ccp(winSize.width * 0.205f, winSize.height * 0.401f));
            auto fallbackMenu = CCMenu::create();
            fallbackMenu->setPosition(CCPointZero);
            fallbackMenu->addChild(btn);
            this->addChild(fallbackMenu, 10);
        }
    }
};
