#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCMenuItemSpriteExtra.hpp>
#include <Geode/modify/CCDirector.hpp>
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

// Màn hình loading phủ kín, giữ nguyên qua các lần đổi scene (che màn hình sảnh).
class LoadingOverlay : public CCLayerColor {
protected:
    CCSprite* m_spinner = nullptr; // có thể null nếu không tìm thấy texture
    CCLabelBMFont* m_label = nullptr;
    bool m_dismissing = false;
    bool m_usesNotificationNode = false;

    bool init() override {
        if (!CCLayerColor::initWithColor(ccc4(0, 0, 0, 255))) return false;

        auto win = CCDirector::get()->getWinSize();

        m_spinner = CCSprite::create("loadingCircle.png");
        if (m_spinner) {
            m_spinner->setScale(0.6f);
            m_spinner->setPosition(ccp(win.width / 2.f, win.height / 2.f + 14.f));
            m_spinner->runAction(CCRepeatForever::create(CCRotateBy::create(1.f, 360.f)));
            this->addChild(m_spinner);
        }

        m_label = CCLabelBMFont::create("Loading level...", "goldFont.fnt");
        m_label->setScale(0.7f);
        m_label->setPosition(ccp(win.width / 2.f, win.height / 2.f - 22.f));
        this->addChild(m_label);

        this->setTouchEnabled(true); // chặn cú chạm xuyên xuống các layer bên dưới
        return true;
    }

    void registerWithTouchDispatcher() override {
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -9999, true);
    }

    void onFaded() {
        if (m_usesNotificationNode) {
            // Gỡ khỏi director một cách an toàn (không để bị huỷ ngay trong lúc đang chạy action)
            this->retain();
            this->onExit();
            CCDirector::get()->setNotificationNode(nullptr);
            this->autorelease();
        } else {
            this->removeFromParentAndCleanup(true);
        }
    }

public:
    static LoadingOverlay* create() {
        auto ret = new LoadingOverlay();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool ccTouchBegan(CCTouch*, CCEvent*) override { return true; }

    void setUsesNotificationNode(bool value) { m_usesNotificationNode = value; }
    bool usesNotificationNode() const { return m_usesNotificationNode; }

    // Hiện dần
    void fadeInNow() {
        this->setOpacity(0);
        m_label->setOpacity(0);
        this->runAction(CCFadeIn::create(0.2f));
        m_label->runAction(CCFadeIn::create(0.2f));
        if (m_spinner) {
            m_spinner->setOpacity(0);
            m_spinner->runAction(CCFadeIn::create(0.2f));
        }
    }

    // Mờ dần rồi tự xoá sau `delay` giây
    void dismiss(float delay) {
        if (m_dismissing) return;
        m_dismissing = true;
        this->setTouchEnabled(false);

        this->runAction(CCSequence::create(
            CCDelayTime::create(delay),
            CCFadeOut::create(0.3f),
            CCCallFunc::create(this, callfunc_selector(LoadingOverlay::onFaded)),
            nullptr
        ));
        m_label->runAction(CCSequence::create(CCDelayTime::create(delay), CCFadeOut::create(0.3f), nullptr));
        if (m_spinner) {
            m_spinner->runAction(CCSequence::create(CCDelayTime::create(delay), CCFadeOut::create(0.3f), nullptr));
        }
    }
};


namespace qs {
    // Tìm PauseLayer đang hiển thị (nằm trong scene hiện tại, tối đa 2 cấp)
    PauseLayer* findPause(CCNode* node, int depth = 0) {
        if (!node || depth > 2) return nullptr;
        if (auto pause = typeinfo_cast<PauseLayer*>(node)) return pause;
        if (auto children = node->getChildren()) {
            for (auto child : CCArrayExt<CCNode*>(children)) {
                if (auto pause = findPause(child, depth + 1)) return pause;
            }
        }
        return nullptr;
    }

    Ref<LoadingOverlay> g_overlay;

    void showLoading() {
        if (g_overlay) return;
        auto director = CCDirector::get();
        auto overlay = LoadingOverlay::create();
        if (!overlay) return;

        if (!director->getNotificationNode()) {
            // Cách chính: gắn vào "notification node" của director. Node này được vẽ đè lên
            // MỌI scene (kể cả lúc đang chuyển cảnh) nên màn loading luôn che kín.
            director->setNotificationNode(overlay);
            overlay->setUsesNotificationNode(true);
            overlay->onEnter();
            overlay->onEnterTransitionDidFinish();
        } else if (auto scene = director->getRunningScene()) {
            // Dự phòng nếu notification node đã bị dùng: gắn vào scene hiện tại
            scene->addChild(overlay, 99999);
        } else {
            return;
        }
        overlay->fadeInNow();
        g_overlay = overlay;
    }

    void hideLoading(float delay) {
        if (g_overlay) {
            g_overlay->dismiss(delay);
            g_overlay = nullptr;
        }
    }

    // Đợi tới khi game đã thoát hẳn level cũ (PlayLayer cũ bị huỷ, hết hiệu ứng chuyển cảnh)
    // rồi mới tạo PlayLayer mới. Mỗi frame kiểm tra một lần.
    void waitAndPlay(Ref<GJGameLevel> level, int frames, bool leaving) {
        Loader::get()->queueInMainThread([level, frames, leaving]() {
            auto director = CCDirector::get();
            bool inTransition = typeinfo_cast<CCTransitionScene*>(director->getRunningScene()) != nullptr;
            bool playLayerAlive = PlayLayer::get() != nullptr;

            if (playLayerAlive || inTransition) {
                bool nowLeaving = leaving || inTransition;
                // Nếu mãi không thoát được (ví dụ đang hiện hộp xác nhận thoát) thì huỷ để không kẹt
                if ((!nowLeaving && frames > 120) || frames > 900) {
                    hideLoading(0.f);
                    return;
                }
                waitAndPlay(level, frames + 1, nowLeaving);
                return;
            }

            // Level cũ đã bị huỷ hẳn: vào level mới (không dùng hiệu ứng, màn loading đang che)
            auto scene = PlayLayer::scene(level, false, false);
            director->replaceScene(scene);

            // Sang frame sau mới cho màn loading mờ dần (lúc đó nó đã được chuyển sang scene mới)
            Loader::get()->queueInMainThread([]() { hideLoading(0.35f); });
        });
    }

    void startLevel(GJGameLevel* level) {
        // Bật màn hình loading che toàn bộ quá trình.
        showLoading();

        // Thoát level hiện tại bằng đúng đường thoát của game (nút Exit trong menu tạm dừng),
        // để mọi mod khác dọn dẹp bình thường. Sau đó mới vào level mới.
        if (auto pause = findPause(CCDirector::get()->getRunningScene())) {
            pause->onQuit(nullptr);
        } else if (auto playLayer = PlayLayer::get()) {
            playLayer->onQuit();
        }
        waitAndPlay(level, 0, false);
    }

    void switchToTarget() {
        if (g_overlay) return; // đang chuyển level rồi
        int id = targetId();
        if (id <= 0) {
            Notification::create("No level selected. Use the + button in the level page.", NotificationIcon::Warning)->show();
            return;
        }

        // Ghi nhớ level đang chơi để lần giữ nút tiếp theo quay lại được level này.
        int curId = 0;
        std::string curName;
        if (auto pl = PlayLayer::get(); pl && pl->m_level) {
            curId = pl->m_level->m_levelID.value();
            curName = std::string(pl->m_level->m_levelName);
        }
        if (curId == id) {
            Notification::create("You are already playing the selected level", NotificationIcon::Warning)->show();
            return;
        }

        bool async = LevelFetcher::fetch(
            id,
            [curId, curName, id](GJGameLevel* level) {
                // Hoán đổi: level đang chơi trở thành level đích mới (chỉ khi nó là level online)
                if (curId > 0 && curId != id) {
                    setTarget(curId, curName);
                }
                startLevel(level);
            },
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

// Mỗi lần game đổi scene, chuyển màn loading sang scene mới để nó không bị mất.
class $modify(QSDirector, CCDirector) {
    void willSwitchToScene(CCScene* scene) {
        CCDirector::willSwitchToScene(scene);
        if (qs::g_overlay && !qs::g_overlay->usesNotificationNode()
            && scene && qs::g_overlay->getParent() != scene) {
            qs::g_overlay->removeFromParentAndCleanup(false);
            scene->addChild(qs::g_overlay, 99999);
        }
    }
};


// ---------------------------------------------------------------------------
// Bộ đếm giữ nút (có vòng tiến trình). Được gắn làm con của một nút THẬT của game
// (CCMenuItemSpriteExtra) để chắc chắn nhận được cú chạm.
// ---------------------------------------------------------------------------
class HoldTimer : public CCNode {
protected:
    std::function<void()> m_onHold;
    CCProgressTimer* m_ring = nullptr;
    float m_holdTime = 0.5f;
    float m_elapsed = 0.f;
    bool m_running = false;

    bool init(CCSprite* ringSprite, float holdTime, std::function<void()> onHold, float scale) {
        if (!CCNode::init()) return false;

        m_holdTime = std::max(holdTime, 0.05f);
        m_onHold = std::move(onHold);

        ringSprite->setOpacity(170);
        m_ring = CCProgressTimer::create(ringSprite);
        m_ring->setType(kCCProgressTimerTypeRadial);
        m_ring->setPercentage(0.f);
        m_ring->setScale(scale);
        this->addChild(m_ring);
        return true;
    }

public:
    static HoldTimer* create(CCSprite* ringSprite, float holdTime, std::function<void()> onHold, float scale) {
        auto ret = new HoldTimer();
        if (ret->init(ringSprite, holdTime, std::move(onHold), scale)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void begin() {
        m_running = true;
        m_elapsed = 0.f;
        this->scheduleUpdate();
    }

    void cancel() {
        m_running = false;
        m_elapsed = 0.f;
        this->unscheduleUpdate();
        m_ring->setPercentage(0.f);
    }

    void update(float dt) override {
        if (!m_running) return;
        m_elapsed += dt;
        m_ring->setPercentage(std::min(100.f, m_elapsed / m_holdTime * 100.f));
        if (m_elapsed >= m_holdTime) {
            m_running = false;
            this->unscheduleUpdate();
            auto cb = m_onHold;
            if (cb) cb(); // có thể thay scene, nên không đụng gì thêm sau dòng này
        }
    }
};

// Hook nút của game: khi nút có HoldTimer con thì bắt đầu/huỷ đếm theo cú chạm.
class $modify(QSMenuItem, CCMenuItemSpriteExtra) {
    void selected() {
        CCMenuItemSpriteExtra::selected();
        if (auto timer = typeinfo_cast<HoldTimer*>(this->getChildByID("hold-timer"_spr))) {
            timer->begin();
        }
    }

    void unselected() {
        CCMenuItemSpriteExtra::unselected();
        if (auto timer = typeinfo_cast<HoldTimer*>(this->getChildByID("hold-timer"_spr))) {
            timer->cancel();
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
    void onQuickSwitchNoop(CCObject*) {}

    void customSetup() {
        PauseLayer::customSetup();

        constexpr float kScale = 0.55f;

        // Dùng icon play có sẵn của game, thu nhỏ lại
        auto icon = qs::frameSprite({"GJ_playBtn2_001.png"});
        icon->setScale(kScale);
        auto btn = CCMenuItemSpriteExtra::create(icon, this, menu_selector(QSPauseLayer::onQuickSwitchNoop));
        btn->setID("quick-switch-hold-button"_spr);

        auto timer = HoldTimer::create(
            qs::frameSprite({"GJ_playBtn2_001.png"}),
            qs::holdTime(),
            []() { qs::requestSwitch(); },
            kScale
        );
        if (timer) {
            timer->setID("hold-timer"_spr);
            auto size = btn->getContentSize();
            timer->setPosition(ccp(size.width / 2.f, size.height / 2.f));
            btn->addChild(timer, 10);
        }

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
