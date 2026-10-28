#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

using namespace geode::prelude;

/*
 * Quick Level Switcher — integration scaffold
 *
 * This file deliberately does NOT pretend the online-level download and
 * gameplay transition pipeline is finished. The UI hooks below are safe
 * discovery points; complete online lookup/download and long-press handling
 * must be connected to the exact generated bindings for the installed
 * Geode 5 SDK and GD 2.2081.
 */

namespace qls {
    constexpr char const* kHoldTimeSetting = "hold-time";
    constexpr char const* kWarnSetting = "warn-switch";

    void showNotImplementedNotice() {
        FLAlertLayer::create(
            "Quick Level Switcher",
            "The UI hook is installed, but online level lookup/download and "
            "the long-press level transition are not wired up in this "
            "prototype yet. See README.md before extending it.",
            "OK"
        )->show();
    }

    // Uses a game-provided sprite frame; no custom image asset is drawn.
    // If this frame name differs in a particular texture pack/build, replace
    // it with a frame name confirmed in the local game's sprite sheets.
    CCMenuItemSpriteExtra* makeGameSpriteButton(
        char const* frameName,
        CCObject* target,
        SEL_MenuHandler callback,
        float scale = 0.65f
    ) {
        auto sprite = CCSprite::createWithSpriteFrameName(frameName);
        if (!sprite) {
            log::warn("Quick Level Switcher: sprite frame '{}' was not found", frameName);
            return nullptr;
        }

        sprite->setScale(scale);
        return CCMenuItemSpriteExtra::create(sprite, target, callback);
    }
}

// Level details screen: add a plus button to open the management entry point.
class $modify(QLS_LevelInfoLayer, LevelInfoLayer) {
    void onEnter() {
        LevelInfoLayer::onEnter();

        // Avoid duplicate buttons if another mod causes onEnter more than once.
        if (this->getChildByID("qls-level-info-menu")) return;

        auto menu = CCMenu::create();
        if (!menu) return;
        menu->setID("qls-level-info-menu");
        menu->setPosition(CCPointZero);

        // Existing Geometry Dash plus-button frame. Verify locally if a
        // texture pack changes/renames sprite frames.
        auto button = qls::makeGameSpriteButton(
            "GJ_plusBtn_001.png",
            this,
            menu_selector(QLS_LevelInfoLayer::onQLSOpen),
            0.65f
        );

        if (button) {
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            button->setPosition({winSize.width * 0.87f, winSize.height * 0.47f});
            menu->addChild(button);
            this->addChild(menu, 100);
        }
    }

    void onQLSOpen(CCObject*) {
        qls::showNotImplementedNotice();
    }
};

// Pause screen: add a game-sprite button as the future long-press switch control.
// The current prototype intentionally does not switch levels on a normal click.
class $modify(QLS_PauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        if (this->getChildByID("qls-pause-menu")) return;

        auto menu = CCMenu::create();
        if (!menu) return;
        menu->setID("qls-pause-menu");
        menu->setPosition(CCPointZero);

        // Reuses a play-button frame from the game's own sprite sheets.
        auto button = qls::makeGameSpriteButton(
            "GJ_playBtn2_001.png",
            this,
            menu_selector(QLS_PauseLayer::onQLSAttemptSwitch),
            0.42f
        );

        if (button) {
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            button->setPosition({winSize.width * 0.205f, winSize.height * 0.50f});
            menu->addChild(button);
            this->addChild(menu, 100);
        }
    }

    void onQLSAttemptSwitch(CCObject*) {
        qls::showNotImplementedNotice();
    }
};
