/**
 * @file aspanelcelestialposition.h
 * @brief Compact sun and moon position controls for AyaneStorm settings panels.
 * @author chanayane@firestorm
 */

#ifndef AS_PANELCELESTIALPOSITION_H
#define AS_PANELCELESTIALPOSITION_H

#include "llenvironment.h"
#include "llpanel.h"
#include "llsettingssky.h"

class LLUICtrl;

class ASPanelCelestialPosition : public LLPanel
{
public:
    ASPanelCelestialPosition() = default;
    ~ASPanelCelestialPosition() override;

    bool postBuild() override;
    void onVisibilityChange(bool new_visibility) override;

private:
    enum class Body { SUN, MOON };

    void refreshPosition();
    void onPositionChanged();
    LLSettingsSky::ptr_t getEditableSky();

    Body mBody = Body::SUN;
    LLUICtrl* mAzimuth = nullptr;
    LLUICtrl* mElevation = nullptr;
    LLSettingsSky::ptr_t mLiveSky;
    LLEnvironment::connection_t mEnvironmentConnection;
    bool mUpdating = false;
};

#endif // AS_PANELCELESTIALPOSITION_H
