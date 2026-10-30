#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/LevelPage.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCMenuItemSpriteExtra.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Dữ liệu: mỗi level (nguồn) có một level đích riêng
// ---------------------------------------------------------------------------
namespace qs {
    struct Target {
        char type = 0;   // 'm' = level chính thức của RobTop, 'o' = level online
        int id = 0;
        std::string name;
        bool valid() const { return type != 0 && id > 0; }
    };

    std::string keyOf(char type, int id) { return std::string(1, type) + std::to_string(id); }
    std::string keyOf(Target const& t) { return keyOf(t.type, t.id); }

    // Chuyển một GJGameLevel thành Target. Level tự tạo trong máy (không có ID) không hỗ trợ.
    Target fromLevel(GJGameLevel* level) {
        Target t;
        if (!level) return t;
        int id = level->m_levelID.value();
        if (id <= 0) return t;
        t.id = id;
        // Kiểu level của game: 1 = level chính thức của RobTop, 2 = level tự tạo, 3 = level online đã lưu
        t.type = (static_cast<int>(level->m_levelType) == 1) ? 'm' : 'o';
        t.name = std::string(level->m_levelName);
        return t;
    }

    std::string shorten(std::string s, size_t maxLen = 26) {
        if (s.size() > maxLen) s = s.substr(0, maxLen) + "...";
        return s;
    }

    // --- cài đặt chung ---
    float holdTime() { return static_cast<float>(Mod::get()->getSavedValue<double>("hold-time", 0.5)); }
    bool warn() { return Mod::get()->getSavedValue<bool>("warn-switch", false); }
    void setHoldTime(float t) { Mod::get()->setSavedValue<double>("hold-time", t); }
    void setWarn(bool b) { Mod::get()->setSavedValue<bool>("warn-switch", b); }

    // --- bảng level nguồn -> level đích (lưu dạng văn bản: key \t type \t id \t name) ---
    std::vector<std::string> splitTabs(std::string const& line) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : line) {
            if (c == '\t') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        parts.push_back(cur);
        return parts;
    }

    std::map<std::string, Target> loadTargets() {
        std::map<std::string, Target> out;
        std::istringstream in(Mod::get()->getSavedValue<std::string>("level-targets", std::string()));
        std::string line;
        while (std::getline(in, line)) {
            auto parts = splitTabs(line);
            if (parts.size() < 4 || parts[1].empty()) continue;
            Target t;
            t.type = parts[1][0];
            try { t.id = std::stoi(parts[2]); } catch (...) { continue; }
            t.name = parts[3];
            if (t.valid()) out[parts[0]] = t;
        }
        return out;
    }

    void saveTargets(std::map<std::string, Target> const& targets) {
        std::string text;
        for (auto const& [key, t] : targets) {
            std::string name = t.name;
            for (auto& c : name) {
                if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            }
            text += key + "\t" + std::string(1, t.type) + "\t" + std::to_string(t.id) + "\t" + name + "\n";
        }
        Mod::get()->setSavedValue<std::string>("level-targets", text);
    }

    Target getTargetFor(std::string const& sourceKey) {
        auto targets = loadTargets();
        auto it = targets.find(sourceKey);
        return it != targets.end() ? it->second : Target{};
    }

    void setTargetFor(std::string const& sourceKey, Target const& t) {
        auto targets = loadTargets();
        targets[sourceKey] = t;
        saveTargets(targets);
    }

    void removeTargetFor(std::string const& sourceKey) {
        auto targets = loadTargets();
        targets.erase(sourceKey);
        saveTargets(targets);
    }

    // Level vừa rời đi bằng nút chuyển: khi đang ở level `g_returnKey` mà level đó chưa có
    // level đích riêng thì giữ nút sẽ quay về `g_returnTarget`. Chỉ lưu trong phiên chơi.
    std::string g_returnKey;
    Target g_returnTarget;

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
// Màn hình loading riêng: thay cho màn hình sảnh khi thoát level để chuyển level
// ---------------------------------------------------------------------------
namespace qs {
    enum class SwitchState { Idle, Quitting, Loading };
    SwitchState g_state = SwitchState::Idle;
    Ref<GJGameLevel> g_pending;
    CCScene* g_oldScene = nullptr; // chỉ dùng để so sánh con trỏ, không giữ tham chiếu
}

// Nằm trong màn loading: chờ game thoát hẳn level cũ rồi mới dựng level mới.
class LoadingRunner : public CCNode {
protected:
    Ref<GJGameLevel> m_level;
    int m_frames = 0;

    bool init(GJGameLevel* level) {
        if (!CCNode::init()) return false;
        m_level = level;
        this->scheduleUpdate();
        return true;
    }

public:
    static LoadingRunner* create(GJGameLevel* level) {
        auto ret = new LoadingRunner();
        if (ret->init(level)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void update(float) override {
        m_frames++;
        auto director = CCDirector::get();
        bool busy = PlayLayer::get() != nullptr
            || typeinfo_cast<CCTransitionScene*>(director->getRunningScene()) != nullptr;
        if (busy && m_frames < 300) return;

        this->unscheduleUpdate();
        log::info("[QuickLevelSwitch] LoadingRunner: building new level after {} frames", m_frames);
        qs::g_state = qs::SwitchState::Idle;
        qs::g_pending = nullptr;

        auto scene = PlayLayer::scene(m_level, false, false);
        director->replaceScene(CCTransitionFade::create(0.3f, scene));
    }
};

namespace qs {
    CCScene* makeLoadingScene(GJGameLevel* level) {
        auto scene = CCScene::create();
        auto win = CCDirector::get()->getWinSize();

        auto bg = CCLayerColor::create(ccc4(0, 0, 0, 255));
        scene->addChild(bg);

        if (auto spinner = CCSprite::create("loadingCircle.png")) {
            spinner->setScale(0.6f);
            spinner->setPosition(ccp(win.width / 2.f, win.height / 2.f + 14.f));
            spinner->runAction(CCRepeatForever::create(CCRotateBy::create(1.f, 360.f)));
            scene->addChild(spinner);
        }

        auto label = CCLabelBMFont::create("Loading level...", "goldFont.fnt");
        label->setScale(0.7f);
        label->setPosition(ccp(win.width / 2.f, win.height / 2.f - 22.f));
        scene->addChild(label);

        if (auto runner = LoadingRunner::create(level)) {
            scene->addChild(runner);
        }
        return scene;
    }
}

// Khi đang chuyển level, scene mà game định vào sau khi thoát level (màn sảnh...) được thay
// bằng màn loading, nên không bao giờ thấy màn sảnh.
class $modify(QSDirector, CCDirector) {
    void replaceScene(CCScene* scene) {
        if (qs::g_state == qs::SwitchState::Quitting && qs::g_pending) {
            log::info("[QuickLevelSwitch] replaceScene intercepted, showing loading scene");
            qs::g_state = qs::SwitchState::Loading;
            auto loading = qs::makeLoadingScene(qs::g_pending);
            CCDirector::replaceScene(CCTransitionFade::create(0.3f, loading));
            return;
        }
        CCDirector::replaceScene(scene);
    }
};

// ---------------------------------------------------------------------------
// Chuyển sang level đích
// ---------------------------------------------------------------------------
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

    // Mỗi frame kiểm tra xem game đã bắt đầu rời level chưa. Khi game bắt đầu chuyển sang scene
    // khác (màn sảnh...), thay luôn điểm đến bằng màn loading. Cách này không phụ thuộc game dùng
    // hàm chuyển scene nào. Nếu game không thoát level (ví dụ đang hiện hộp xác nhận thoát) thì
    // huỷ sau ~10 giây.
    void pollQuit(int frames) {
        Loader::get()->queueInMainThread([frames]() {
            if (g_state != SwitchState::Quitting) return;

            auto director = CCDirector::get();
            if (g_pending && director->getRunningScene() != g_oldScene) {
                log::info("[QuickLevelSwitch] game started leaving the level (after {} frames), showing loading scene", frames);
                g_state = SwitchState::Loading;
                director->replaceScene(makeLoadingScene(g_pending));
                return;
            }

            if (frames > 600) {
                log::info("[QuickLevelSwitch] timed out waiting for the game to leave the level");
                g_state = SwitchState::Idle;
                g_pending = nullptr;
                return;
            }
            pollQuit(frames + 1);
        });
    }

    void startLevel(GJGameLevel* level) {
        auto pause = findPause(CCDirector::get()->getRunningScene());
        auto playLayer = PlayLayer::get();
        log::info("[QuickLevelSwitch] startLevel: pauseLayer found={}, playLayer found={}", pause != nullptr, playLayer != nullptr);
        if (!pause && !playLayer) return;

        g_pending = level;
        g_oldScene = CCDirector::get()->getRunningScene();
        g_state = SwitchState::Quitting;

        // Thoát level bằng đúng đường thoát của game để mọi mod khác dọn dẹp bình thường.
        // Lúc game định chuyển về màn sảnh, hook QSDirector sẽ đổi sang màn loading.
        if (pause) {
            pause->onQuit(nullptr);
        } else {
            playLayer->onQuit();
        }
        pollQuit(0);
    }

    // Trả về true nếu phải tải qua mạng (bất đồng bộ)
    bool loadTarget(Target const& t, std::function<void(GJGameLevel*)> onDone, std::function<void()> onFail) {
        if (t.type == 'm') {
            auto level = GameLevelManager::get()->getMainLevel(t.id, false);
            if (level) onDone(level); else onFail();
            return false;
        }
        return LevelFetcher::fetch(t.id, std::move(onDone), std::move(onFail));
    }

    void switchToTarget() {
        if (g_state != SwitchState::Idle) return;

        auto playLayer = PlayLayer::get();
        Target cur = (playLayer && playLayer->m_level) ? fromLevel(playLayer->m_level) : Target{};
        if (!cur.valid()) {
            Notification::create("This level type is not supported (only official and online levels)", NotificationIcon::Warning)->show();
            return;
        }
        std::string curKey = keyOf(cur);

        // Ưu tiên level đích do người chơi đặt riêng cho level này;
        // nếu không có thì quay về level vừa rời đi bằng nút chuyển.
        Target target = getTargetFor(curKey);
        if (!target.valid() && g_returnKey == curKey && g_returnTarget.valid()) {
            target = g_returnTarget;
        }
        if (!target.valid()) {
            Notification::create("No level set for this one. Open its level page and press +", NotificationIcon::Warning)->show();
            return;
        }
        if (keyOf(target) == curKey) {
            Notification::create("You are already playing that level", NotificationIcon::Warning)->show();
            return;
        }

        log::info("[QuickLevelSwitch] switching from {} to {}", curKey, keyOf(target));
        bool async = loadTarget(
            target,
            [cur, target](GJGameLevel* level) {
                // Nhớ đường về: từ level đích, nếu chưa có level đích riêng thì giữ nút sẽ quay lại level này
                g_returnKey = keyOf(target);
                g_returnTarget = cur;
                startLevel(level);
            },
            []() { Notification::create("Could not load the level", NotificationIcon::Error)->show(); }
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
// Menu cài đặt: level đích của MỘT level cụ thể, thời gian giữ, cảnh báo
// ---------------------------------------------------------------------------
class QuickSwitchPopup : public Popup {
protected:
    qs::Target m_src;
    TextInput* m_idInput = nullptr;
    TextInput* m_holdInput = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemToggler* m_officialToggle = nullptr;
    CCMenuItemToggler* m_warnToggle = nullptr;
    CCMenuItemSpriteExtra* m_okBtn = nullptr;
    bool m_busy = false;

    bool init(qs::Target const& src) {
        if (!Popup::init(390.f, 250.f)) return false;
        m_src = src;
        this->setTitle("Quick Level Switch");

        auto existing = qs::getTargetFor(qs::keyOf(src));

        // Level đang được cài đặt
        auto forLabel = CCLabelBMFont::create(
            fmt::format("For: {}", qs::shorten(src.name)).c_str(), "chatFont.fnt"
        );
        forLabel->setScale(0.7f);
        m_mainLayer->addChildAtPosition(forLabel, Anchor::Center, ccp(0.f, 80.f));

        // Hàng 1: ô nhập ID, nút xoá, nút OK
        m_idInput = TextInput::create(170.f, "Enter level ID");
        m_idInput->setCommonFilter(CommonFilter::Uint);
        m_idInput->setMaxCharCount(10);
        if (existing.valid()) {
            m_idInput->setString(std::to_string(existing.id));
        }
        m_mainLayer->addChildAtPosition(m_idInput, Anchor::Center, ccp(-45.f, 45.f));

        auto trashSpr = CCSprite::createWithSpriteFrameName("GJ_trashBtn_001.png");
        trashSpr->setScale(0.8f);
        auto trashBtn = CCMenuItemSpriteExtra::create(trashSpr, this, menu_selector(QuickSwitchPopup::onClear));
        m_buttonMenu->addChildAtPosition(trashBtn, Anchor::Center, ccp(65.f, 45.f));

        auto okSpr = ButtonSprite::create("OK");
        m_okBtn = CCMenuItemSpriteExtra::create(okSpr, this, menu_selector(QuickSwitchPopup::onOk));
        m_buttonMenu->addChildAtPosition(m_okBtn, Anchor::Center, ccp(135.f, 45.f));

        // Hàng 2: level chính thức của RobTop hay level online
        m_officialToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(QuickSwitchPopup::onNoop), 0.6f
        );
        m_officialToggle->toggle(existing.valid() && existing.type == 'm');
        m_buttonMenu->addChildAtPosition(m_officialToggle, Anchor::Center, ccp(-165.f, 12.f));

        auto officialLabel = CCLabelBMFont::create("Official level (Stereo Madness = 1, ...)", "chatFont.fnt");
        officialLabel->setScale(0.65f);
        officialLabel->setAnchorPoint(ccp(0.f, 0.5f));
        m_mainLayer->addChildAtPosition(officialLabel, Anchor::Center, ccp(-148.f, 12.f));

        // Hàng 3: level đích hiện tại
        m_status = CCLabelBMFont::create("", "chatFont.fnt");
        m_status->setScale(0.7f);
        m_mainLayer->addChildAtPosition(m_status, Anchor::Center, ccp(0.f, -15.f));
        this->refreshStatus();

        // Hàng 4: thời gian giữ
        auto holdLabel = CCLabelBMFont::create("Hold time (seconds):", "chatFont.fnt");
        holdLabel->setScale(0.7f);
        holdLabel->setAnchorPoint(ccp(0.f, 0.5f));
        m_mainLayer->addChildAtPosition(holdLabel, Anchor::Center, ccp(-170.f, -50.f));

        m_holdInput = TextInput::create(60.f, "0.5");
        m_holdInput->setCommonFilter(CommonFilter::Float);
        m_holdInput->setMaxCharCount(5);
        m_holdInput->setString(fmt::format("{:.2f}", qs::holdTime()));
        m_mainLayer->addChildAtPosition(m_holdInput, Anchor::Center, ccp(-10.f, -50.f));

        // Hàng 5: cảnh báo
        m_warnToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(QuickSwitchPopup::onNoop), 0.6f
        );
        m_warnToggle->toggle(qs::warn());
        m_buttonMenu->addChildAtPosition(m_warnToggle, Anchor::Center, ccp(-165.f, -88.f));

        auto warnLabel = CCLabelBMFont::create("Warn before switching levels", "chatFont.fnt");
        warnLabel->setScale(0.7f);
        warnLabel->setAnchorPoint(ccp(0.f, 0.5f));
        m_mainLayer->addChildAtPosition(warnLabel, Anchor::Center, ccp(-148.f, -88.f));

        return true;
    }

    void refreshStatus() {
        auto existing = qs::getTargetFor(qs::keyOf(m_src));
        std::string text;
        if (existing.valid()) {
            text = fmt::format(
                "Selected: {} ({}{})", qs::shorten(existing.name, 22),
                existing.type == 'm' ? "official " : "", existing.id
            );
        } else {
            text = "No level selected";
        }
        m_status->setString(text.c_str());
    }

    void onNoop(CCObject*) {}

    void onClear(CCObject*) {
        if (m_busy) return;
        m_idInput->setString("");
        qs::removeTargetFor(qs::keyOf(m_src));
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
            qs::removeTargetFor(qs::keyOf(m_src));
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

        bool official = m_officialToggle->isToggled();
        char type = official ? 'm' : 'o';
        if (qs::keyOf(type, id) == qs::keyOf(m_src)) {
            Notification::create("A level cannot switch to itself", NotificationIcon::Warning)->show();
            return;
        }

        if (official) {
            auto level = GameLevelManager::get()->getMainLevel(id, true);
            if (!level || level->m_levelName.empty()) {
                Notification::create("No official level with that ID", NotificationIcon::Error)->show();
                return;
            }
            this->saveTarget(type, id, std::string(level->m_levelName));
            return;
        }

        m_busy = true;
        m_okBtn->setEnabled(false);
        m_status->setString("Loading level...");

        Ref<QuickSwitchPopup> self = this;
        qs::loadTarget(
            qs::Target{ type, id, "" },
            [self, id](GJGameLevel* level) { self->onLoaded(id, level); },
            [self]() { self->onFailed(); }
        );
    }

    void saveTarget(char type, int id, std::string const& name) {
        qs::setTargetFor(qs::keyOf(m_src), qs::Target{ type, id, name });
        Notification::create(fmt::format("Saved: {}", name), NotificationIcon::Success)->show();
        this->keyBackClicked();
    }

    void onLoaded(int id, GJGameLevel* level) {
        this->saveTarget('o', id, std::string(level->m_levelName));
    }

    void onFailed() {
        m_busy = false;
        m_okBtn->setEnabled(true);
        this->refreshStatus();
        Notification::create("Could not download the level", NotificationIcon::Error)->show();
    }

public:
    static QuickSwitchPopup* create(qs::Target const& src) {
        auto ret = new QuickSwitchPopup();
        if (ret->init(src)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

namespace qs {
    void openSettingsFor(GJGameLevel* level) {
        auto src = fromLevel(level);
        if (!src.valid()) {
            Notification::create("This level type is not supported (only official and online levels)", NotificationIcon::Warning)->show();
            return;
        }
        if (auto popup = QuickSwitchPopup::create(src)) {
            popup->show();
        }
    }
}

// ---------------------------------------------------------------------------
// Màn hình sảnh level online: thêm nút dấu cộng
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
        qs::openSettingsFor(m_level);
    }
};

// ---------------------------------------------------------------------------
// Màn hình chọn level chính thức: mỗi trang level có một nút dấu cộng đi theo trang
// ---------------------------------------------------------------------------
class $modify(QSLevelPage, LevelPage) {
    bool init(GJGameLevel* level) {
        if (!LevelPage::init(level)) return false;

        auto winSize = CCDirector::get()->getWinSize();

        auto spr = qs::frameSprite({"GJ_plusBtn_001.png"});
        spr->setScale(0.8f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(QSLevelPage::onQuickSwitch));
        btn->setID("quick-switch-button"_spr);

        // Góc trên bên trái của khung level, tính trong toạ độ của trang
        CCPoint pagePos = ccp(winSize.width / 2.f - 197.f, 247.f);

        if (m_levelMenu) {
            // Đổi sang toạ độ của menu có sẵn của trang (nút trong menu này nhận cú chạm bình thường
            // khi trang đang được cuộn)
            auto world = this->convertToWorldSpace(pagePos);
            btn->setPosition(m_levelMenu->convertToNodeSpace(world));
            m_levelMenu->addChild(btn);
        } else {
            auto menu = CCMenu::create();
            menu->setPosition(CCPointZero);
            btn->setPosition(pagePos);
            menu->addChild(btn);
            this->addChild(menu, 10);
        }

        this->refreshQuickSwitch();
        return true;
    }

    void updateDynamicPage(GJGameLevel* level) {
        LevelPage::updateDynamicPage(level);
        this->refreshQuickSwitch();
    }

    // Chỉ hiện dấu cộng ở các trang là level chính thức (không hiện ở trang "The Tower"...)
    void refreshQuickSwitch() {
        auto node = m_levelMenu ? m_levelMenu->getChildByID("quick-switch-button"_spr) : nullptr;
        if (!node) node = this->getChildByIDRecursive("quick-switch-button"_spr);
        if (!node) return;
        auto t = qs::fromLevel(m_level);
        node->setVisible(t.valid() && t.type == 'm' && t.id >= 1 && t.id <= 22);
    }

    void onQuickSwitch(CCObject*) {
        qs::openSettingsFor(m_level);
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
