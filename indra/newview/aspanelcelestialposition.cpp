/**
 * @file aspanelcelestialposition.cpp
 * @brief Compact sun and moon position controls for AyaneStorm settings panels.
 * @author chanayane@firestorm
 */

#include "llviewerprecompiledheaders.h"

#include "aspanelcelestialposition.h"

#include "llenvironment.h"
#include "llui.h"
#include "lluictrl.h"
#include "llvirtualtrackball.h"
#include "pipeline.h"

namespace
{
    // LLEnvironment::NO_VERSION is -3 and is used by Quick Preferences.
    // Keep our self-update marker distinct so preset replacements are observed.
    constexpr S32 AS_CELESTIAL_POSITION_UPDATE = -5;
}

static LLPanelInjector<ASPanelCelestialPosition> t_as_celestial_position("as_celestial_position_panel");

ASPanelCelestialPosition::~ASPanelCelestialPosition()
{
    mEnvironmentConnection.disconnect();
}

bool ASPanelCelestialPosition::postBuild()
{
    // getChild<T>() creates a dummy widget when a name is absent, so it cannot
    // be used to distinguish the Sun and Moon variants of this reusable panel.
    mAzimuth = dynamic_cast<LLUICtrl*>(findChildView("as_sun_azimuth", false));
    mElevation = dynamic_cast<LLUICtrl*>(findChildView("as_sun_elevation", false));
    if (!mAzimuth || !mElevation)
    {
        mBody = Body::MOON;
        mAzimuth = dynamic_cast<LLUICtrl*>(findChildView("as_moon_azimuth", false));
        mElevation = dynamic_cast<LLUICtrl*>(findChildView("as_moon_elevation", false));
    }

    if (!mAzimuth || !mElevation)
    {
        LL_WARNS("AyaneStorm") << "Celestial position panel has no position sliders" << LL_ENDL;
        return false;
    }

    mAzimuth->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPositionChanged(); });
    mElevation->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPositionChanged(); });

    // Personal Lighting edits a fixed local sky. Establish the same source as
    // soon as this panel opens instead of sampling the interpolated render sky.
    mLiveSky = getEditableSky();
    mEnvironmentConnection = LLEnvironment::instance().setEnvironmentChanged(
        [this](LLEnvironment::EnvSelection_t env, S32 version)
        {
            if (env == LLEnvironment::ENV_LOCAL && version != AS_CELESTIAL_POSITION_UPDATE)
            {
                mLiveSky = getEditableSky();
                refreshPosition();
            }
        });
    // Environment listeners may pause reflection updates (SL-20456).
    gPipeline.mReflectionMapManager.resume();
    refreshPosition();
    return true;
}

void ASPanelCelestialPosition::onVisibilityChange(bool new_visibility)
{
    LLPanel::onVisibilityChange(new_visibility);
    if (new_visibility)
    {
        refreshPosition();
    }
}

void ASPanelCelestialPosition::refreshPosition()
{
    if (mUpdating || !mAzimuth || !mElevation)
    {
        return;
    }

    if (!mLiveSky)
    {
        mAzimuth->setEnabled(false);
        mElevation->setEnabled(false);
        return;
    }

    F32 azimuth;
    F32 elevation;
    const LLQuaternion rotation =
        mBody == Body::SUN ? mLiveSky->getSunRotation() : mLiveSky->getMoonRotation();
    LLVirtualTrackball::getAzimuthAndElevationDeg(rotation, azimuth, elevation);

    mUpdating = true;
    mAzimuth->setEnabled(true);
    mElevation->setEnabled(true);
    mAzimuth->setValue(azimuth);
    mElevation->setValue(elevation);
    mUpdating = false;
}

LLSettingsSky::ptr_t ASPanelCelestialPosition::getEditableSky()
{
    LLEnvironment& environment = LLEnvironment::instance();
    LLSettingsSky::ptr_t sky;

    if (environment.hasEnvironment(LLEnvironment::ENV_LOCAL))
    {
        sky = environment.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
        if (environment.getEnvironmentDay(LLEnvironment::ENV_LOCAL) && sky)
        {
            // Freeze the local day at its current frame, as Personal Lighting does.
            sky = sky->buildClone();
            environment.setEnvironment(LLEnvironment::ENV_LOCAL, sky, AS_CELESTIAL_POSITION_UPDATE);
        }
    }
    else
    {
        // Personal Lighting starts from the parcel's effective fixed sky.
        const LLSettingsSky::ptr_t parcel_sky =
            environment.getEnvironmentFixedSky(LLEnvironment::ENV_PARCEL, true);
        if (!parcel_sky)
        {
            return nullptr;
        }
        sky = parcel_sky->buildClone();
        environment.setEnvironment(LLEnvironment::ENV_LOCAL, sky, AS_CELESTIAL_POSITION_UPDATE);
    }

    environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    return sky;
}

void ASPanelCelestialPosition::onPositionChanged()
{
    if (mUpdating)
    {
        return;
    }

    if (!mLiveSky)
    {
        mLiveSky = getEditableSky();
    }
    if (!mLiveSky)
    {
        return;
    }

    F32 azimuth = (F32)mAzimuth->getValue().asReal() * DEG_TO_RAD;
    F32 elevation = (F32)mElevation->getValue().asReal() * DEG_TO_RAD;
    if (is_approx_zero(elevation))
    {
        elevation = F_APPROXIMATELY_ZERO;
    }

    LLQuaternion rotation;
    rotation.setAngleAxis(-elevation, 0.f, 1.f, 0.f);
    LLQuaternion azimuth_rotation;
    azimuth_rotation.setAngleAxis(F_TWO_PI - azimuth, 0.f, 0.f, 1.f);
    rotation *= azimuth_rotation;

    if (mBody == Body::SUN)
    {
        mLiveSky->setSunRotation(rotation);
    }
    else
    {
        mLiveSky->setMoonRotation(rotation);
    }
    mLiveSky->update();
}
