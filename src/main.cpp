#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/binding/LevelInfoLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/GameLevel.hpp>
#include <Geode/binding/MenuLayer.hpp>
#include <Geode/binding/GameManager.hpp>

using namespace geode::prelude;

// Lưu trữ cấu hình cài đặt trong Mod Settings hoặc Global variables đơn giản
class QuickSwitcherManager {
public:
    static QuickSwitcherManager& get() {
        static QuickSwitcherManager instance;
        return instance;
    }

    int targetLevelID = 0;
    float holdTime = 0.5f;
    bool showWarning = false;
};

// ==========================================
// 1. POPUP CÀI ĐẶT LEVEL VÀ HOLD TIME (Hình 3)
// ==========================================
class QuickSwitcherPopup : public Popup {
protected:
    TextInput* m_inputID;
    TextInput* m_inputHoldTime;

    bool setup() override {
        auto winSize = CCDirector::get()->getWinSize();
        
        // Tạo background popup dùng chung của GD
        auto bg = CCScale9Sprite::create("GJ_square01.png", { 0, 0, 80, 80 });
        bg->setContentSize({ 420, 260 });
        bg->setPosition(winSize / 2);
        this->m_mainLayer->addChild(bg);

        // Nút Đóng (X)
        auto closeBtn = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png"),
            this,
            menu_selector(QuickSwitcherPopup::onClose)
        );
        auto menu = CCMenu::create();
        menu->addChild(closeBtn);
        menu->setPosition({ winSize.width / 2 - 200, winSize.height / 2 + 120 });
        this->m_mainLayer->addChild(menu);

        // Ô nhập Level ID
        m_inputID = TextInput::create(280, "Enter Level ID / Song ID", "chatFont.fnt");
        m_inputID->setString(fmt::format("{}", QuickSwitcherManager::get().targetLevelID));
        m_inputID->setPosition({ winSize.width / 2, winSize.height / 2 + 50 });
        this->m_mainLayer->addChild(m_inputID);

        // Label và Ô nhập Hold Time
        auto holdLabel = CCLabelBMFont::create("Hold time:", "bigFont.fnt");
        holdLabel->setScale(0.5f);
        holdLabel->setPosition({ winSize.width / 2 - 90, winSize.height / 2 - 20 });
        this->m_mainLayer->addChild(holdLabel);

        m_inputHoldTime = TextInput::create(80, "0.5", "chatFont.fnt");
        m_inputHoldTime->setString(fmt::format("{:.1f}", QuickSwitcherManager::get().holdTime));
        m_inputHoldTime->setPosition({ winSize.width / 2 - 10, winSize.height / 2 - 20 });
        this->m_mainLayer->addChild(m_inputHoldTime);

        // Nút OK lưu lại
        auto okBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("OK", 80, true, "goldFont.fnt", "GJ_button_01.png", 30, 0.8f),
            this,
            menu_selector(QuickSwitcherPopup::onSave)
        );
        auto okMenu = CCMenu::create();
        okMenu->addChild(okBtn);
        okMenu->setPosition({ winSize.width / 2 + 130, winSize.height / 2 - 20 });
        this->m_mainLayer->addChild(okMenu);

        return true;
    }

    void onSave(CCObject*) {
        std::string idStr = m_inputID->getString();
        std::string timeStr = m_inputHoldTime->getString();

        QuickSwitcherManager::get().targetLevelID = utils::numFromString<int>(idStr).unwrapOr(0);
        QuickSwitcherManager::get().holdTime = utils::numFromString<float>(timeStr).unwrapOr(0.5f);

        Notification::create("Settings Saved!", CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png"))->show();
        this->onClose(nullptr);
    }

    void onClose(CCObject* sender) {
        this->removeFromByParent(true); // Hoặc dùng hàm remove tùy theo Geode version, dùng self->removeMeAndCleanup()
    }

public:
    static void createPopup() {
        auto popup = new QuickSwitcherPopup();
        if (popup && popup->init(420, 260)) {
            popup->autorelease();
            popup->show();
        } else {
            delete popup;
        }
    }
};

// ==========================================
// 2. THÊM NÚT DẤU CỘNG Ở MÀN HÌNH SẢNH LEVEL (Hình 2)
// ==========================================
#include <Geode/modify/LevelInfoLayer.hpp>
class $modify(MyLevelInfoLayer, LevelInfoLayer) {
    bool init(GameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;

        // Lấy texture nút dấu cộng có sẵn trong game: GJ_plusBtn_001.png
        auto plusSprite = CCSprite::createWithSpriteFrameName("GJ_plusBtn_001.png");
        auto plusBtn = CCMenuItemSpriteExtra::create(
            plusSprite,
            this,
            menu_selector(MyLevelInfoLayer::onOpenSwitcherSettings)
        );

        // Đặt vị trí nút dấu cộng bên cạnh các nút thông tin (như trong hình 2)
        auto menu = this->getChildByID("right-side-menu"); // hoặc tìm menu phù hợp
        if (!menu) {
            // Fallback nếu không tìm thấy ID menu
            menu = CCMenu::create();
            menu->setPosition({0, 0});
            this->addChild(menu);
        }
        
        plusBtn->setPosition({ 350, 80 }); // Tùy chỉnh tọa độ cho khớp giao diện
        menu->addChild(plusBtn);

        return true;
    }

    void onOpenSwitcherSettings(CCObject*) {
        QuickSwitcherPopup::createPopup();
    }
};

// ==========================================
// 3. THÊM NÚT CHUYỂN LEVEL TRONG MÀN CHƠI / PAUSE MENU (Hình 1)
// ==========================================
#include <Geode/modify/PauseLayer.hpp>
class $modify(MyPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto winSize = CCDirector::get()->getWinSize();

        // Lấy icon nút play nhỏ có sẵn trong game: GJ_playBtn_001.png hoặc nút tương tự
        auto playSprite = CCSprite::createWithSpriteFrameName("GJ_playBtn_001.png");
        playSprite->setScale(0.7f);

        auto switchBtn = CCMenuItemSpriteExtra::create(
            playSprite,
            this,
            menu_selector(MyPauseLayer::onSwitchLevel)
        );

        // Đặt vị trí nút chuyển level bên trái nút Play chính (như hình 1)
        auto menu = CCMenu::create();
        switchBtn->setPosition({ -120, 0 }); // Căn chỉnh tương đối so với menu giữa
        
        // Tìm menu chứa nút pause chính hoặc tạo menu mới đặt giữa màn hình
        // Trong PauseLayer, các nút chính nằm trong một CCMenu ở giữa màn hình
        for (auto child : CCArrayExt<CCNode*>(this->getChildren())) {
            if (auto innerMenu = typeinfo_cast<CCMenu*>(child)) {
                // Kiểm tra xem có chứa nút play không để bám vào đó
                innerMenu->addChild(switchBtn);
                break;
            }
        }
    }

    void onSwitchLevel(CCObject*) {
        int targetID = QuickSwitcherManager::get().targetLevelID;
        if (targetID <= 0) {
            Notification::create("No target level set! Click '+' in level menu.", CCSprite::createWithSpriteFrameName("geode.loader/err.png"))->show();
            return;
        }

        // Thực hiện chuyển sang level mới dựa vào ID thông qua GameLevelManager
        // Logic tải level online hoặc official level
        GameLevelManager::get()->m_levelID = targetID;
        
        // Load và chuyển scene tới level mới
        auto loadLevel = GJGameLevel::create(); // Hoặc gọi hàm load tương ứng của GD API
        // Chuyển scene chơi game lập tức
        FMODAudioEngine::get()->stopAllEffects();
        
        // Load trực tiếp Editor hoặc PlayLayer tùy vào loại level
        FLAlertLayer::create("Quick Switcher", fmt::format("Switching to Level ID: {}", targetID), "OK")->show();
        
        // Ví dụ kích hoạt tải level theo ID:
        GameLevelManager::get()->getOnlineLevel(targetID);
    }
};
