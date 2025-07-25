/**
 * @file llviewertexturelist.cpp
 * @brief Object for managing the list of images within a region
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include <sys/stat.h>

#include "llviewertexturelist.h"

#include "llagent.h"
#include "llgl.h" // fot gathering stats from GL
#include "llimagegl.h"
#include "llimagebmp.h"
#include "llimagej2c.h"
#include "llimagetga.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llimageworker.h"

#include "llsdserialize.h"
#include "llsys.h"
#include "llfilesystem.h"
#include "llxmltree.h"
#include "message.h"

#include "lldrawpoolbump.h" // to init bumpmap images
#include "lltexturecache.h"
#include "lltexturefetch.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewermedia.h"
#include "llviewernetwork.h"
#include "llviewerregion.h"
#include "llviewerstats.h"
#include "pipeline.h"
#include "llappviewer.h"
#include "llxuiparser.h"
#include "lltracerecording.h"
#include "llviewerdisplay.h"
#include "llviewerwindow.h"
#include "llprogressview.h"

////////////////////////////////////////////////////////////////////////////

void (*LLViewerTextureList::sUUIDCallback)(void **, const LLUUID&) = NULL;

S32 LLViewerTextureList::sNumImages = 0;

// <FS:Ansariel> Fast cache stats
U32 LLViewerTextureList::sNumFastCacheReads = 0;

LLViewerTextureList gTextureList;

extern LLGLSLShader gCopyProgram;

ETexListType get_element_type(S32 priority)
{
    return (priority == LLViewerFetchedTexture::BOOST_ICON || priority == LLViewerFetchedTexture::BOOST_THUMBNAIL) ? TEX_LIST_SCALE : TEX_LIST_STANDARD;
}

///////////////////////////////////////////////////////////////////////////////

LLTextureKey::LLTextureKey()
: textureId(LLUUID::null),
textureType(TEX_LIST_STANDARD)
{
}

LLTextureKey::LLTextureKey(LLUUID id, ETexListType tex_type)
: textureId(id), textureType(tex_type)
{
}

///////////////////////////////////////////////////////////////////////////////

LLViewerTextureList::LLViewerTextureList()
    : mForceResetTextureStats(false),
    mInitialized(false)
{
}

void LLViewerTextureList::init()
{
    mInitialized = true ;
    sNumImages = 0;
    doPreloadImages();
}


void LLViewerTextureList::doPreloadImages()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_DEBUGS("ViewerImages") << "Preloading images..." << LL_ENDL;

    llassert_always(mInitialized) ;
    llassert_always(mImageList.empty()) ;
    llassert_always(mUUIDMap.empty()) ;

    // Set the "missing asset" image
    LLViewerFetchedTexture::sMissingAssetImagep = LLViewerTextureManager::getFetchedTextureFromFile("missing_asset.tga", FTT_LOCAL_FILE, MIPMAP_NO, LLViewerFetchedTexture::BOOST_UI);

    // Set the "white" image
    LLViewerFetchedTexture::sWhiteImagep = LLViewerTextureManager::getFetchedTextureFromFile("white.tga", FTT_LOCAL_FILE, MIPMAP_NO, LLViewerFetchedTexture::BOOST_UI);
    LLTexUnit::sWhiteTexture = LLViewerFetchedTexture::sWhiteImagep->getTexName();
    LLUIImageList* image_list = LLUIImageList::getInstance();

    // Set default particle texture
    LLViewerFetchedTexture::sDefaultParticleImagep = LLViewerTextureManager::getFetchedTextureFromFile("pixiesmall.j2c");

    // Set the default flat normal map
    // BLANK_OBJECT_NORMAL has a version on dataserver, but it has compression artifacts
    LLViewerFetchedTexture::sFlatNormalImagep =
        LLViewerTextureManager::getFetchedTextureFromFile("flatnormal.tga",
                                                          FTT_LOCAL_FILE,
                                                          MIPMAP_NO,
                                                          LLViewerFetchedTexture::BOOST_BUMP,
                                                          LLViewerTexture::FETCHED_TEXTURE,
                                                          0,
                                                          0,
                                                          BLANK_OBJECT_NORMAL);

    // PBR: irradiance
    LLViewerFetchedTexture::sDefaultIrradiancePBRp = LLViewerTextureManager::getFetchedTextureFromFile("default_irradiance.png", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);

    image_list->initFromFile();

    // turn off clamping and bilinear filtering for uv picking images
    //LLViewerFetchedTexture* uv_test = preloadUIImage("uv_test1.tga", LLUUID::null, false);
    //uv_test->setClamp(false, false);
    //uv_test->setMipFilterNearest(true, true);
    //uv_test = preloadUIImage("uv_test2.tga", LLUUID::null, false);
    //uv_test->setClamp(false, false);
    //uv_test->setMipFilterNearest(true, true);

    LLViewerFetchedTexture* image = LLViewerTextureManager::getFetchedTextureFromFile("silhouette.j2c", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("world/NoEntryLines.png", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("world/NoEntryPassLines.png", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("transparent.j2c", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI, LLViewerTexture::FETCHED_TEXTURE,
        0, 0, IMG_TRANSPARENT);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("alpha_gradient.tga", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI, LLViewerTexture::FETCHED_TEXTURE,
        GL_ALPHA8, GL_ALPHA, IMG_ALPHA_GRAD);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_CLAMP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("alpha_gradient_2d.j2c", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI, LLViewerTexture::FETCHED_TEXTURE,
        GL_ALPHA8, GL_ALPHA, IMG_ALPHA_GRAD_2D);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_CLAMP);
        mImagePreloads.insert(image);
    }
}

static std::string get_texture_list_name()
{
    // <FS:Ansariel> OpenSim compatibility
    //if (LLGridManager::getInstance()->isInProductionGrid())
    if (LLGridManager::getInstance()->isInSLMain())
    // </FS:Ansariel>
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE,
            "texture_list_" + gSavedSettings.getString("LoginLocation") + "." + gDirUtilp->getUserName() + ".xml");
    }
    else
    {
        const std::string& grid_id_str = LLGridManager::getInstance()->getGridId();
        const std::string& grid_id_lower = utf8str_tolower(grid_id_str);
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE,
            "texture_list_" + gSavedSettings.getString("LoginLocation") + "." + gDirUtilp->getUserName() + "." + grid_id_lower + ".xml");
    }
}

void LLViewerTextureList::doPrefetchImages()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    // todo: do not load without getViewerAssetUrl()
    // either fail login without caps or provide this
    // in some other way, textures won't load otherwise
    LLViewerFetchedTexture *imagep = findImage(DEFAULT_WATER_NORMAL, TEX_LIST_STANDARD);
    if (!imagep)
    {
        // add it to mImagePreloads only once
        imagep = LLViewerTextureManager::getFetchedTexture(DEFAULT_WATER_NORMAL, FTT_DEFAULT, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
        if (imagep)
        {
            imagep->setAddressMode(LLTexUnit::TAM_WRAP);
            mImagePreloads.insert(imagep);
        }
    }

    LLViewerTextureManager::getFetchedTexture(IMG_SHOT);
    LLViewerTextureManager::getFetchedTexture(IMG_SMOKE_POOF);
    LLViewerFetchedTexture::sSmokeImagep = LLViewerTextureManager::getFetchedTexture(IMG_SMOKE, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
    LLViewerFetchedTexture::sSmokeImagep->setNoDelete();

    LLStandardBumpmap::addstandard();

    if (LLAppViewer::instance()->getPurgeCache())
    {
        // cache was purged, no point
        return;
    }

    // Pre-fetch textures from last logout
    LLSD imagelist;
    std::string filename = get_texture_list_name();
    llifstream file;
    file.open(filename.c_str());
    if (file.is_open())
    {
        if ( ! LLSDSerialize::fromXML(imagelist, file) )
        {
            file.close();
            LL_WARNS() << "XML parse error reading texture list '" << filename << "'" << LL_ENDL;
            LL_WARNS() << "Removing invalid texture list '" << filename << "'" << LL_ENDL;
            LLFile::remove(filename);
            return;
        }
        file.close();
    }
    S32 texture_count = 0;
    for (LLSD::array_iterator iter = imagelist.beginArray();
         iter != imagelist.endArray(); ++iter)
    {
        LLSD imagesd = *iter;
        LLUUID uuid = imagesd["uuid"];
        S32 pixel_area = imagesd["area"];
        S32 texture_type = imagesd["type"];

        if((LLViewerTexture::FETCHED_TEXTURE == texture_type || LLViewerTexture::LOD_TEXTURE == texture_type) && !LLViewerTexture::isInvisiprim(uuid))
        {
            LLViewerFetchedTexture* image = LLViewerTextureManager::getFetchedTexture(uuid, FTT_DEFAULT, MIPMAP_TRUE, LLGLTexture::BOOST_NONE, texture_type);
            if (image)
            {
                texture_count += 1;
                image->addTextureStats((F32)pixel_area);
            }
        }
    }
    LL_DEBUGS() << "fetched " << texture_count << " images from " << filename << LL_ENDL;
}

///////////////////////////////////////////////////////////////////////////////

LLViewerTextureList::~LLViewerTextureList()
{
}

void LLViewerTextureList::shutdown()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_WARNS() << "Shutdown called" << LL_ENDL;
    // clear out preloads
    mImagePreloads.clear();

    // Write out list of currently loaded textures for precaching on startup
    typedef std::set<std::pair<S32,LLViewerFetchedTexture*> > image_area_list_t;
    image_area_list_t image_area_list;
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); ++iter)
    {
        LLViewerFetchedTexture* image = *iter;
        if (!image->hasGLTexture() ||
            !image->getUseDiscard() ||
            image->needsAux() ||
            !image->getTargetHost().isInvalid() ||
            !image->getUrl().empty() ||
            image->isInvisiprim()
            )
        {
            continue; // avoid UI, baked, and other special images
        }
        if(!image->getBoundRecently())
        {
            continue ;
        }
        S32 desired = image->getDesiredDiscardLevel();
        if (desired >= 0 && desired < MAX_DISCARD_LEVEL)
        {
            S32 pixel_area = image->getWidth(desired) * image->getHeight(desired);
            image_area_list.insert(std::make_pair(pixel_area, image));
        }
    }

    LLSD imagelist;
    const S32 max_count = 1000;
    S32 count = 0;
    S32 image_type ;
    for (image_area_list_t::reverse_iterator riter = image_area_list.rbegin();
         riter != image_area_list.rend(); ++riter)
    {
        LLViewerFetchedTexture* image = riter->second;
        image_type = (S32)image->getType() ;
        imagelist[count]["area"] = riter->first;
        imagelist[count]["uuid"] = image->getID();
        imagelist[count]["type"] = image_type;
        if (++count >= max_count)
            break;
    }

    if (count > 0 && !gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "").empty())
    {
        std::string filename = get_texture_list_name();
        llofstream file;
        file.open(filename.c_str());
        LL_DEBUGS() << "saving " << imagelist.size() << " image list entries" << LL_ENDL;
        LLSDSerialize::toPrettyXML(imagelist, file);
    }

    //
    // Clean up "loaded" callbacks.
    //
    mCallbackList.clear();

    // Flush all of the references
    while (!mCreateTextureList.empty())
    {
        mCreateTextureList.front()->mCreatePending = false;
        mCreateTextureList.pop();
    }
    mFastCacheList.clear();

    mUUIDMap.clear();

    mImageList.clear();

    mInitialized = false ; //prevent loading textures again.
}
// <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings
// static
// Allows the menu to call the dump method of the texture list
void LLViewerTextureList::dumpTexturelist()
{
    gTextureList.dump();
}
// </FS:minerjr> [FIRE-35081]

void LLViewerTextureList::dump()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_INFOS() << "LLViewerTextureList::dump()" << LL_ENDL;
    // <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings
    S32 texture_count = 0;
    S32 textures_close_to_camera = 0;
    std::array<S32, MAX_DISCARD_LEVEL * 2 + 2> image_counts{0}; // Double the size for higher discards from textures < 1024 (2048 can make a 7 and 4096 could make an 8)
    std::array<S32, 12 * 12> size_counts{0}; // Track the 12 possible sizes (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048)
    std::array<S32, (MAX_DISCARD_LEVEL * 2 + 2) * 12> discard_counts{0}; // Also need to an 1 additional as -1 is a valid discard level (not loaded by reported as a 1x1 texture)
    std::array<S32, (MAX_DISCARD_LEVEL * 2 + 2) * 12> fullsize_discard_counts{0}; // Also need to an 1 additional as -1 is a valid discard level (not loaded by reported as a 1x1 texture)
    std::array<S32, LLViewerTexture::BOOST_MAX_LEVEL * 12> boost_counts{0}; // Track the # of textures at boost levels by 12 possible sizes
    // Don't Init the buffers with 0's like it's the the 1980's...
    
    // </FS:minerjr> [FIRE-35081]
    for (image_list_t::iterator it = mImageList.begin(); it != mImageList.end(); ++it)
    {
        LLViewerFetchedTexture* image = *it;
        LL_INFOS() << "priority " << image->getMaxVirtualSize()
        << " boost " << image->getBoostLevel()
        << " size " << image->getWidth() << "x" << image->getHeight()
        << " discard " << image->getDiscardLevel()
        << " desired " << image->getDesiredDiscardLevel()
        // <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings
        << " close to camera " << (image->getCloseToCamera() > 0.0f ? "Y" : "N") // Display the close to camera flag
        << " FFType " << fttype_to_string(image->getFTType()) // Display the FFType of the camera
        << " Type " << (S32)image->getType() // Display the type of the image (LOCAL_TEXTURE = 0, MEDIA_TEXTURE = 1, DYNAMIC_TEXTURE = 2, FETCHED_TEXTURE = 3,LOD_TEXTURE = 4)        
        << " Sculpted " << (image->forSculpt() ? "Y" : "N")
        << " # of Faces ";
        for (S32 index = 0; index < LLRender::NUM_TEXTURE_CHANNELS; index++)
        {
            LL_CONT << image->getNumFaces(index) << " ";
        }        
        LL_CONT << " # of Volumes ";
        for (S32 index = 0; index < LLRender::NUM_VOLUME_TEXTURE_CHANNELS; index++)
        {
            LL_CONT << image->getNumVolumes(index) << " ";
        }
        // </FS:minerjr> [FIRE-35081]
        LL_CONT << " " << image->getID().asString().substr(0, 7)
        << LL_ENDL;
        // <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings
        image_counts[(image->getDiscardLevel() + 1)] += 1; // Need to add +1 to make up for -1 being a possible value
        S32 x_index = (S32)log2(image->getWidth()); // Convert the width into a 0 based index by taking the Log2 of the size to get the exponent of the size. (1 = 2^0, 2 = 2^1, 4 = 2^2...)
        S32 y_index = (S32)log2(image->getHeight()); // Convert the height into a 0 based index by taking the Log2 of the size to get the exponent of the size. (1 = 2^0, 2 = 2^1, 4 = 2^2...)
        size_counts[x_index + y_index * 12] += 1; // Add this texture's dimensions to the size count
        // Onlyuse the largest size for the texture's discard level(for non-square textures)
        S32 max_dimension = (y_index > x_index ? y_index : x_index);
        discard_counts[(image->getDiscardLevel() + 1) + max_dimension * (MAX_DISCARD_LEVEL * 2 + 2)] += 1;
        boost_counts[image->getBoostLevel() + max_dimension * (LLViewerTexture::BOOST_MAX_LEVEL)] += 1;
        S32 full_x_index = (S32)log2(image->getFullWidth());
        S32 full_y_index = (S32)log2(image->getFullHeight());
        S32 full_max_dimension = (full_y_index > full_x_index ? full_y_index : full_x_index);
        fullsize_discard_counts[(image->getDiscardLevel() + 1) + full_max_dimension * (MAX_DISCARD_LEVEL * 2 + 2)] += 1;
        texture_count++;
        textures_close_to_camera += S32(image->getCloseToCamera());
        // </FS:minerjr> [FIRE-35081]
    }
    // <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings
    // Add overal texture totals
    LL_INFOS() << "Texture Stats: Textures in Close to Camera " << textures_close_to_camera << " of " << texture_count << " : " << LL_ENDL;

    // Fix for the -1 discard level as well as higher possible discard levels (for 2048+ size textures)
    for (S32 index = 0; index < MAX_DISCARD_LEVEL * 2 + 2; index++)
    {
        LL_INFOS() << " Discard Level: " << (index - 1) << " Number of Textures: " << image_counts[index] << LL_ENDL;
    }

    // Create a line to break up the header from the content of the table
    std::string header_break(13 * 8, '-');

    LL_INFOS() << "Size vs Size" << LL_ENDL;
    LL_INFOS() << header_break << LL_ENDL;
    // Create a header that for the Sizes
    LL_INFOS() << std::setw(8) << "Size";
    for (S32 x = 1; x <= 2048; x <<= 1)
    {
        LL_CONT << std::setw(8) << x;
    }
    LL_CONT << LL_ENDL;
    LL_INFOS() << header_break << LL_ENDL;

    // Y Axis is the size of the height of the texture
    for (S32 y = 0; y < 12; y++)
    {
        LL_INFOS() << std::setw(8) << (1 << y);
        //X Axis is the size of the width of the texture
        for (S32 x = 0; x < 12; x++)
        {
            LL_CONT << std::setw(8) << size_counts[x + y * 12];

        }
        LL_CONT << LL_ENDL;
    }
    LL_INFOS() << LL_ENDL;

    // This is the Discard Level Vs Size counts table
    LL_INFOS() << "Discard Level Vs Size" << LL_ENDL;
    LL_INFOS() << header_break << LL_ENDL;
    LL_INFOS() << std::setw(8) << "Discard";
    for (S32 x = 0; x < MAX_DISCARD_LEVEL * 2 + 2; x++)
    {
        LL_CONT << std::setw(8) << (x - 1);
    }
    LL_CONT << LL_ENDL;
    LL_INFOS() << header_break << LL_ENDL;

    // Y Axis is the current possible max dimension of the textures (X or Y, which ever is larger is used)
    for (S32 y = 0; y < 12; y++)
    {
        LL_INFOS() << std::setw(8) << (1 << y);
        // X Axis is the discard level starging from -1 up to 10 (2 x MAX_DISCARD_LEVEL + 1 (for negative number) + 1 additional for the fact that the last value actuauly used on not < but <=)
        for (S32 x = 0; x < (MAX_DISCARD_LEVEL * 2 + 2); x++)
        {
            LL_CONT << std::setw(8) << discard_counts[x + y * (MAX_DISCARD_LEVEL * 2 + 2)];
        }
        LL_CONT << LL_ENDL;
    }
    LL_INFOS() << LL_ENDL;
    

    // This is the Discard Level Vs Full Size counts table
    LL_INFOS() << "Discard Level Vs Full Size" << LL_ENDL;
    LL_INFOS() << header_break << LL_ENDL;
    LL_INFOS() << std::setw(8) << "Discard";
    for (S32 x = 0; x < MAX_DISCARD_LEVEL * 2 + 2; x++)
    {
        LL_CONT << std::setw(8) << (x - 1);
    }
    LL_CONT << LL_ENDL;
    LL_INFOS() << header_break << LL_ENDL;

    // Y Axis is the current possible max dimension of the textures (X or Y, which ever is larger is used)
    for (S32 y = 0; y < 12; y++)
    {
        LL_INFOS() << std::setw(8) << (1 << y);
        // X Axis is the discard level starging from -1 up to 10 (2 x MAX_DISCARD_LEVEL + 1 (for negative number) + 1 additional for the fact that the last value actuauly used on not < but <=)
        for (S32 x = 0; x < (MAX_DISCARD_LEVEL * 2 + 2); x++)
        {
            LL_CONT << std::setw(8) << fullsize_discard_counts[x + y * (MAX_DISCARD_LEVEL * 2 + 2)];
        }
        LL_CONT << LL_ENDL;
    }
    LL_INFOS() << LL_ENDL;


    // This is the Boost Level Vs Size counts table
    LL_INFOS() << "Boost Level Vs Size" << LL_ENDL;
    header_break.append((LLViewerTexture::BOOST_MAX_LEVEL * 8) - (12 * 8), '-');
    LL_INFOS() << header_break << LL_ENDL;    
    LL_INFOS() << std::setw(8) << "Discard";
    for (S32 x = 0; x < LLViewerTexture::BOOST_MAX_LEVEL; x++)
    {
        LL_CONT << std::setw(8) << x;
    }
    LL_CONT << LL_ENDL;
    LL_INFOS() << header_break << LL_ENDL;

    // Y Axis is the current possible max dimension of the textures (X or Y, which ever is larger is used)
    for (S32 y = 0; y < 12; y++)
    {
        LL_INFOS() << std::setw(8) << (1 << y);
        // X Axis is the boost level starging from BOOST_NONE up to BOOST_MAX_LEVEL
        for (S32 x = 0; x < (LLViewerTexture::BOOST_MAX_LEVEL); x++)
        {
            LL_CONT << std::setw(8) << boost_counts[x + y * (LLViewerTexture::BOOST_MAX_LEVEL)];
        }
        LL_CONT << LL_ENDL;
    }
    LL_INFOS() << LL_ENDL;

    // </FS:minerjr> [FIRE-35081]
}

void LLViewerTextureList::destroyGL()
{
    LLImageGL::destroyGL();
}

/* Vertical tab container button image IDs
 Seem to not decode when running app in debug.

 const LLUUID BAD_IMG_ONE("1097dcb3-aef9-8152-f471-431d840ea89e");
 const LLUUID BAD_IMG_TWO("bea77041-5835-1661-f298-47e2d32b7a70");
 */

///////////////////////////////////////////////////////////////////////////////

LLViewerFetchedTexture* LLViewerTextureList::getImageFromFile(const std::string& filename,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   LLGLint internal_format,
                                                   LLGLenum primary_format,
                                                   const LLUUID& force_id)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_PROFILE_ZONE_TEXT(filename.c_str(), filename.size());
    if(!mInitialized)
    {
        return NULL ;
    }

    std::string full_path = gDirUtilp->findSkinnedFilename("textures", filename);
    if (full_path.empty())
    {
        LL_WARNS() << "Failed to find local image file: " << filename << LL_ENDL;
        LLViewerTexture::EBoostLevel priority = LLGLTexture::BOOST_UI;
        return LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT, FTT_DEFAULT, true, priority);
    }

    std::string url = "file://" + full_path;

    return getImageFromUrl(url, f_type, usemipmaps, boost_priority, texture_type, internal_format, primary_format, force_id);
}

LLViewerFetchedTexture* LLViewerTextureList::getImageFromUrl(const std::string& url,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   LLGLint internal_format,
                                                   LLGLenum primary_format,
                                                   const LLUUID& force_id)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!mInitialized)
    {
        return NULL ;
    }

    // generate UUID based on hash of filename
    LLUUID new_id;
    if (force_id.notNull())
    {
        new_id = force_id;
    }
    else
    {
        new_id.generate(url);
    }

    LLPointer<LLViewerFetchedTexture> imagep = findImage(new_id, get_element_type(boost_priority));

    if (!imagep.isNull())
    {
        LLViewerFetchedTexture *texture = imagep.get();
        if (texture->getUrl().empty())
        {
            LL_WARNS() << "Requested texture " << new_id << " already exists but does not have a URL" << LL_ENDL;
        }
        else if (texture->getUrl() != url)
        {
            // This is not an error as long as the images really match -
            // e.g. could be two avatars wearing the same outfit.
            LL_DEBUGS("Avatar") << "Requested texture " << new_id
                                << " already exists with a different url, requested: " << url
                                << " current: " << texture->getUrl() << LL_ENDL;
        }

    }
    if (imagep.isNull())
    {
        switch(texture_type)
        {
        case LLViewerTexture::FETCHED_TEXTURE:
            imagep = new LLViewerFetchedTexture(url, f_type, new_id, usemipmaps);
            break ;
        case LLViewerTexture::LOD_TEXTURE:
            imagep = new LLViewerLODTexture(url, f_type, new_id, usemipmaps);
            break ;
        default:
            LL_ERRS() << "Invalid texture type " << texture_type << LL_ENDL ;
        }

        if (internal_format && primary_format)
        {
            imagep->setExplicitFormat(internal_format, primary_format);
        }

        addImage(imagep, get_element_type(boost_priority));

        if (boost_priority != 0)
        {
            if (boost_priority == LLViewerFetchedTexture::BOOST_UI)
            {
                imagep->dontDiscard();
            }
            if (boost_priority == LLViewerFetchedTexture::BOOST_ICON
                || boost_priority == LLViewerFetchedTexture::BOOST_THUMBNAIL)
            {
                // Agent and group Icons are downloadable content, nothing manages
                // icon deletion yet, so they should not persist
                imagep->dontDiscard();
                imagep->forceActive();
            }
            imagep->setBoostLevel(boost_priority);
        }
    }

    imagep->setGLTextureCreated(true);

    return imagep;
}

LLImageRaw* LLViewerTextureList::getRawImageFromMemory(const U8* data, U32 size, std::string_view mimetype)
{
    LLPointer<LLImageFormatted> image = LLImageFormatted::loadFromMemory(data, size, mimetype);

    if (image)
    {
        LLImageRaw* raw_image = new LLImageRaw();
        image->decode(raw_image, 0.f);
        return raw_image;
    }
    else
    {
        return nullptr;
    }
}

LLViewerFetchedTexture* LLViewerTextureList::getImageFromMemory(const U8* data, U32 size, std::string_view mimetype)
{
    LLPointer<LLImageRaw> raw_image = getRawImageFromMemory(data, size, mimetype);
    if (raw_image.notNull())
    {
        LLViewerFetchedTexture* imagep = new LLViewerFetchedTexture(raw_image, FTT_LOCAL_FILE, true);
        addImage(imagep, TEX_LIST_STANDARD);

        imagep->dontDiscard();
        imagep->setBoostLevel(LLViewerFetchedTexture::BOOST_PREVIEW);
        return imagep;
    }
    else
    {
        return nullptr;
    }
}

LLViewerFetchedTexture* LLViewerTextureList::getImage(const LLUUID &image_id,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   LLGLint internal_format,
                                                   LLGLenum primary_format,
                                                   LLHost request_from_host)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!mInitialized)
    {
        return NULL ;
    }

    // Return the image with ID image_id
    // If the image is not found, creates new image and
    // enqueues a request for transmission

    if (image_id.isNull())
    {
        return (LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT, FTT_DEFAULT, true, LLGLTexture::BOOST_UI));
    }

    LLPointer<LLViewerFetchedTexture> imagep = findImage(image_id, get_element_type(boost_priority));
    if (!imagep.isNull())
    {
        LLViewerFetchedTexture *texture = imagep.get();
        if (request_from_host.isOk() &&
            !texture->getTargetHost().isOk())
        {
            LL_WARNS() << "Requested texture " << image_id << " already exists but does not have a host" << LL_ENDL;
        }
        else if (request_from_host.isOk() &&
                 texture->getTargetHost().isOk() &&
                 request_from_host != texture->getTargetHost())
        {
            LL_WARNS() << "Requested texture " << image_id << " already exists with a different target host, requested: "
                    << request_from_host << " current: " << texture->getTargetHost() << LL_ENDL;
        }
        if (f_type != FTT_DEFAULT && imagep->getFTType() != f_type)
        {
            LL_WARNS() << "FTType mismatch: requested " << f_type << " image has " << imagep->getFTType() << LL_ENDL;
        }

    }
    if (imagep.isNull())
    {
        imagep = createImage(image_id, f_type, usemipmaps, boost_priority, texture_type, internal_format, primary_format, request_from_host) ;
    }

    imagep->setGLTextureCreated(true);

    return imagep;
}

//when this function is called, there is no such texture in the gTextureList with image_id.
LLViewerFetchedTexture* LLViewerTextureList::createImage(const LLUUID &image_id,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   LLGLint internal_format,
                                                   LLGLenum primary_format,
                                                   LLHost request_from_host)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static LLCachedControl<bool> fast_cache_fetching_enabled(gSavedSettings, "FastCacheFetchEnabled", true); // <FS:Ansariel> Keep Fast Cache option

    LLPointer<LLViewerFetchedTexture> imagep ;
    switch(texture_type)
    {
    case LLViewerTexture::FETCHED_TEXTURE:
        imagep = new LLViewerFetchedTexture(image_id, f_type, request_from_host, usemipmaps);
        break ;
    case LLViewerTexture::LOD_TEXTURE:
        imagep = new LLViewerLODTexture(image_id, f_type, request_from_host, usemipmaps);
        break ;
    default:
        LL_ERRS() << "Invalid texture type " << texture_type << LL_ENDL ;
    }

    if (internal_format && primary_format)
    {
        imagep->setExplicitFormat(internal_format, primary_format);
    }

    // <FS:minerjr> [FIRE-35428] - Mega prim issue - fix compressed sculpted textures
    // Sculpted textures use the RGBA data for coodinates, so any compression can cause artifacts.
    if (boost_priority == LLViewerFetchedTexture::BOOST_SCULPTED)
    {
        // Disable the compression of BOOST_SCULPTED textures
        if (imagep->getGLTexture())imagep->getGLTexture()->setAllowCompression(false);
    }
    // </FS:minerjr> [FIRE-35428]
    addImage(imagep, get_element_type(boost_priority));

    if (boost_priority != 0)
    {
        if (boost_priority == LLViewerFetchedTexture::BOOST_UI)
        {
            imagep->dontDiscard();
        }
        if (boost_priority == LLViewerFetchedTexture::BOOST_ICON
            || boost_priority == LLViewerFetchedTexture::BOOST_THUMBNAIL)
        {
            // Agent and group Icons are downloadable content, nothing manages
            // icon deletion yet, so they should not persist.
            imagep->dontDiscard();
            imagep->forceActive();
        }
        imagep->setBoostLevel(boost_priority);
    }
    else
    {
        //by default, the texture can not be removed from memory even if it is not used.
        //here turn this off
        //if this texture should be set to NO_DELETE, call setNoDelete() afterwards.
        imagep->forceActive() ;
    }

    // <FS:Ansariel> Keep Fast Cache option
    // <FS:minerjr> [FIRE-35428] - Mega prim issue - fix compressed sculpted textures
    //if(fast_cache_fetching_enabled)
    // If the texture is Sculpted, don't allow it to be added to fast cache as it can affect the texture.
    if(fast_cache_fetching_enabled && boost_priority != LLViewerFetchedTexture::BOOST_SCULPTED)
    // </FS:minerjr> [FIRE-35428]
    {
        mFastCacheList.insert(imagep);
        imagep->setInFastCacheList(true);
    }
    // </FS:Ansariel>
    return imagep ;
}

void LLViewerTextureList::findTexturesByID(const LLUUID &image_id, std::vector<LLViewerFetchedTexture*> &output)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTextureKey search_key(image_id, TEX_LIST_STANDARD);
    uuid_map_t::iterator iter = mUUIDMap.lower_bound(search_key);
    while (iter != mUUIDMap.end() && iter->first.textureId == image_id)
    {
        output.push_back(iter->second);
        iter++;
    }
}

LLViewerFetchedTexture *LLViewerTextureList::findImage(const LLTextureKey &search_key)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    uuid_map_t::iterator iter = mUUIDMap.find(search_key);
    if (iter == mUUIDMap.end())
        return NULL;
    return iter->second;
}

LLViewerFetchedTexture *LLViewerTextureList::findImage(const LLUUID &image_id, ETexListType tex_type)
{
    return findImage(LLTextureKey(image_id, tex_type));
}

void LLViewerTextureList::addImageToList(LLViewerFetchedTexture *image)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    assert_main_thread();
    llassert_always(mInitialized) ;
    llassert(image);
    if (image->isInImageList())
    {   // Flag is already set?
        LL_WARNS() << "LLViewerTextureList::addImageToList - image " << image->getID()  << " already in list" << LL_ENDL;
    }
    else
    {
        if (!(mImageList.insert(image)).second)
        {
            LL_WARNS() << "Error happens when insert image " << image->getID()  << " into mImageList!" << LL_ENDL ;
        }
        image->setInImageList(true);
    }
}

void LLViewerTextureList::removeImageFromList(LLViewerFetchedTexture *image)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    assert_main_thread();
    llassert_always(mInitialized) ;
    llassert(image);

    size_t count = 0;
    if (image->isInImageList())
    {
        image->setInImageList(false);
        count = mImageList.erase(image) ;
        if(count != 1)
        {
            LL_INFOS() << "Image  " << image->getID()
                << " had mInImageList set but mImageList.erase() returned " << count
                << LL_ENDL;
        }
    }
    else
    {   // Something is wrong, image is expected in list or callers should check first
        LL_INFOS() << "Calling removeImageFromList() for " << image->getID()
            << " but doesn't have mInImageList set"
            << " ref count is " << image->getNumRefs()
            << LL_ENDL;
        uuid_map_t::iterator iter = mUUIDMap.find(LLTextureKey(image->getID(), (ETexListType)image->getTextureListType()));
        if(iter == mUUIDMap.end())
        {
            LL_INFOS() << "Image  " << image->getID() << " is also not in mUUIDMap!" << LL_ENDL ;
        }
        else if (iter->second != image)
        {
            LL_INFOS() << "Image  " << image->getID() << " was in mUUIDMap but with different pointer" << LL_ENDL ;
    }
        else
    {
            LL_INFOS() << "Image  " << image->getID() << " was in mUUIDMap with same pointer" << LL_ENDL ;
        }
        count = mImageList.erase(image) ;
        llassert(count != 0);
        if(count != 0)
        {   // it was in the list already?
            LL_WARNS() << "Image  " << image->getID()
                << " had mInImageList false but mImageList.erase() returned " << count
                << LL_ENDL;
        }
    }
}

void LLViewerTextureList::addImage(LLViewerFetchedTexture *new_image, ETexListType tex_type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!new_image)
    {
        return;
    }
    //llassert(new_image);
    LLUUID image_id = new_image->getID();
    LLTextureKey key(image_id, tex_type);

    LLViewerFetchedTexture *image = findImage(key);
    if (image)
    {
        LL_INFOS() << "Image with ID " << image_id << " already in list" << LL_ENDL;
    }
    sNumImages++;

    addImageToList(new_image);
    mUUIDMap[key] = new_image;
    new_image->setTextureListType(tex_type);
}


void LLViewerTextureList::deleteImage(LLViewerFetchedTexture *image)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if( image)
    {
        if (image->hasCallbacks())
        {
            mCallbackList.erase(image);
        }
        LLTextureKey key(image->getID(), (ETexListType)image->getTextureListType());
        llverify(mUUIDMap.erase(key) == 1);
        sNumImages--;
        removeImageFromList(image);
    }
}

///////////////////////////////////////////////////////////////////////////////


void LLViewerTextureList::updateImages(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static bool cleared = false;
    if(gTeleportDisplay)
    {
        if(!cleared)
        {
            clearFetchingRequests();
            gPipeline.clearRebuildGroups();
            cleared = true;
            return;
        }
        // ARRIVING is a delay to let things decode, cache and process,
        // so process textures like normal despite gTeleportDisplay
        if (gAgent.getTeleportState() != LLAgent::TELEPORT_ARRIVING)
        {
            return;
        }
    }
    else
    {
        cleared = false;
    }

    LLAppViewer::getTextureFetch()->setTextureBandwidth((F32)LLTrace::get_frame_recording().getPeriodMeanPerSec(LLStatViewer::TEXTURE_NETWORK_DATA_RECEIVED).value());

    {
        using namespace LLStatViewer;
        sample(NUM_IMAGES, sNumImages);
        sample(NUM_RAW_IMAGES, LLImageRaw::sRawImageCount);
        sample(FORMATTED_MEM, F64Bytes(LLImageFormatted::sGlobalFormattedMemory));
    }

    // make sure each call below gets at least its "fair share" of time
    F32 min_time = max_time * 0.33f;
    F32 remaining_time = max_time;

    //loading from fast cache
    remaining_time -= updateImagesLoadingFastCache(remaining_time);
    remaining_time = llmax(remaining_time, min_time);

    //dispatch to texture fetch threads
    remaining_time -= updateImagesFetchTextures(remaining_time);
    remaining_time = llmax(remaining_time, min_time);

    //handle results from decode threads
    updateImagesCreateTextures(remaining_time);

    bool didone = false;
    for (image_list_t::iterator iter = mCallbackList.begin();
        iter != mCallbackList.end(); )
    {
        //trigger loaded callbacks on local textures immediately
        LLViewerFetchedTexture* image = *iter++;
        if (!image->getUrl().empty())
        {
            // Do stuff to handle callbacks, update priorities, etc.
            didone = image->doLoadedCallbacks();
        }
        else if (!didone)
        {
            // Do stuff to handle callbacks, update priorities, etc.
            didone = image->doLoadedCallbacks();
        }
    }

    updateImagesUpdateStats();
}

void LLViewerTextureList::clearFetchingRequests()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (LLAppViewer::getTextureFetch()->getNumRequests() == 0)
    {
        return;
    }

    LLAppViewer::getTextureFetch()->deleteAllRequests();

    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); ++iter)
    {
        LLViewerFetchedTexture* imagep = *iter;
        imagep->forceToDeleteRequest() ;
    }
}

extern bool gCubeSnapshot;

void LLViewerTextureList::updateImageDecodePriority(LLViewerFetchedTexture* imagep, bool flush_images)
{
    llassert(!gCubeSnapshot);

    constexpr F32 BIAS_TRS_OUT_OF_SCREEN = 1.5f;
    constexpr F32 BIAS_TRS_ON_SCREEN = 1.f;

    if (imagep->getBoostLevel() < LLViewerFetchedTexture::BOOST_HIGH)  // don't bother checking face list for boosted textures
    {
        static LLCachedControl<F32> texture_scale_min(gSavedSettings, "TextureScaleMinAreaFactor", 0.0095f);
        static LLCachedControl<F32> texture_scale_max(gSavedSettings, "TextureScaleMaxAreaFactor", 25.f);

        F32 max_vsize = 0.f;
        bool on_screen = false;

        U32 face_count = 0;

        // get adjusted bias based on image resolution
        LLImageGL* img = imagep->getGLTexture();
        F32 max_discard = F32(img ? img->getMaxDiscardLevel() : MAX_DISCARD_LEVEL);
        F32 bias = llclamp(max_discard - 2.f, 1.f, LLViewerTexture::sDesiredDiscardBias);

        // convert bias into a vsize scaler
        // <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings
        //bias = (F32) llroundf(powf(4, bias - 1.f));
        // Pre-divide the bias so you can just use multiply in the loop
        bias = (F32) 1.0f / llroundf(powf(4, bias - 1.f));

        // Apply new rules to bias discard, there are now 2 bias, off-screen and on-screen.
        // On-screen Bias
        // Only applied to LOD Textures and one that have Discard > 1 (0, 1 protected)
        // 
        // Off-screen Bias
        // Will be using the old method of applying the mMaxVirtualSize, however
        // only on LOD textures and fetched textures get bias applied.
        // 
        // Local (UI & Icons), Media and Dynamic textures should not have any discard applied to them.
        // 
        // Without this, textures will become blurry that are on screen, which is one of the #1
        // user complaints.

        // Store a seperate max on screen vsize without bias applied.
        F32 max_on_screen_vsize = 0.0f;
        S32 on_screen_count = 0;
        // Moved all the variables outside of the loop
        bool current_on_screen = false;
        F32 vsize = 0.0f; // Moved outside the loop to save reallocation every loop
        F32 important_to_camera = 0.0f;
        F32 close_to_camera = 0.0f; // Track if the texture is close to the cameras
        F64 animated = 0; // U64 used to track if a pointer is set for the animations. (The texture matrix of the face is null if no animation assigned to the texture)
                           // So if you keep adding and the final result is 0, there is no animation
        // </FS:minerjr> [FIRE-35081]
        // boost resolution of textures that are important to the camera
        // Can instead of using max for a min of 1.0, just subtract 1 from the boost and just do a 1 + (TextureCameraBoost - 1) * importanceToCamera)
        static LLCachedControl<F32> texture_camera_boost(gSavedSettings, "TextureCameraBoost", 7.f);
        LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
        for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
        {
            for (S32 fi = 0; fi < imagep->getNumFaces(i); ++fi)
            {
                LLFace* face = (*(imagep->getFaceList(i)))[fi];

                if (face && face->getViewerObject())
                {
                    ++face_count;
                    // <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings
                    // No longer needed as we no longer re-calculate the face's virtual texture size, we use it directly from the face
                    //F32 radius;
                    //F32 cos_angle_to_view_dir;
                    // </FS:minerjr> [FIRE-35081]

                    if ((gFrameCount - face->mLastTextureUpdate) > 10)
                    { // only call calcPixelArea at most once every 10 frames for a given face
                        // this helps eliminate redundant calls to calcPixelArea for faces that have multiple textures
                        // assigned to them, such as is the case with GLTF materials or Blinn-Phong materials

                        //face->calcPixelArea(cos_angle_to_view_dir, radius);
                        // The face already has a function to calculate the Texture Virtual Size, which already calls the calcPixelArea method
                        // so just call this instead. This can be called from outside this loop by LLVolume objects
                        face->getTextureVirtualSize();
                        face->mLastTextureUpdate = gFrameCount;
                    }
                                        
                    // Also moved allocation outside the loop
                    // <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings
                    //F32 vsize = face->getPixelArea();

                    //on_screen |= face->mInFrustum;
                    // Get the already calculated face's virtual size, instead of re-calculating it
                    vsize = face->getVirtualSize();
                    
                    current_on_screen = face->mInFrustum; // Create a new var to store the current on screen status                    
                    on_screen_count += current_on_screen; // Count the number of on sceen faces instead of using brach
                    important_to_camera = face->mImportanceToCamera; // Store so we don't have to do 2 indirects later on
                    // If the face/texture is animated, then set the boost level to high, so that it will ways be the best quality
                    animated += S64(face->mTextureMatrix);
                    animated += S64(face->hasMedia()); // Add has media for both local and parcel media
                    animated += S64(imagep->hasParcelMedia());

                    // <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings (It is)
                    /*
                    // Scale desired texture resolution higher or lower depending on texture scale
                    //
                    // Minimum usage examples: a 1024x1024 texture with aplhabet (texture atlas),
                    // runing string shows one letter at a time. If texture has ten 100px symbols
                    // per side, minimal scale is (100/1024)^2 = 0.0095
                    //
                    // Maximum usage examples: huge chunk of terrain repeats texture
                    // TODO: make this work with the GLTF texture transforms

                    S32 te_offset = face->getTEOffset();  // offset is -1 if not inited
                    LLViewerObject* objp = face->getViewerObject();
                    const LLTextureEntry* te = (te_offset < 0 || te_offset >= objp->getNumTEs()) ? nullptr : objp->getTE(te_offset);
                    F32 min_scale = te ? llmin(fabsf(te->getScaleS()), fabsf(te->getScaleT())) : 1.f;
                    min_scale = llclamp(min_scale * min_scale, texture_scale_min(), texture_scale_max());
                    vsize /= min_scale;

                    // apply bias to offscreen faces all the time, but only to onscreen faces when bias is large
                    // use mImportanceToCamera to make bias switch a bit more gradual
                    if (!face->mInFrustum || LLViewerTexture::sDesiredDiscardBias > 1.9f + face->mImportanceToCamera / 2.f)
                    {
                        vsize /= bias;
                    }

                    // boost resolution of textures that are important to the camera
                    if (face->mInFrustum)
                    {
                        static LLCachedControl<F32> texture_camera_boost(gSavedSettings, "TextureCameraBoost", 8.f);
                        vsize *= llmax(face->mImportanceToCamera*texture_camera_boost, 1.f);
                    }

                    max_vsize = llmax(max_vsize, vsize);

                    // addTextureStats limits size to sMaxVirtualSize
                    if (max_vsize >= LLViewerFetchedTexture::sMaxVirtualSize
                        && (on_screen || LLViewerTexture::sDesiredDiscardBias <= BIAS_TRS_ON_SCREEN))
                    {
                        break;
                    }
                    */

                    // Use math to skip having to use a conditaional check
                    // Bools are stored as 0 false, 1 true, use to cheat
                    // Lerp instead of doing conditional input
                    // If the image is import to the camera, even a little then make the on screen true                    
                    on_screen_count += S32(important_to_camera * 1000.0f);
                    //vsize = vsize + (vsize * (1.0f + important_to_camera * texture_camera_boost) - vsize) * F32(current_on_screen);
                    // Apply boost of size based upon importance to camera
                    vsize = vsize + (vsize * important_to_camera * texture_camera_boost);
                    // Apply second boost based upon if the texture is close to the camera (< 16.1 meters * draw distance multiplier)
                    vsize = vsize + (vsize * face->mCloseToCamera * texture_camera_boost);
                    // Update the max on screen vsize based upon the on screen vsize
                    close_to_camera += face->mCloseToCamera;
                    // LL_DEBUGS() << face->getViewerObject()->getID() << " TID " << imagep->getID() << " #F " << imagep->getNumFaces(i) << " OS Vsize: " << vsize << " Vsize: " << (vsize * bias) << " CTC: " << face->mCloseToCamera << " Channel " << i << " Face Index " << fi << LL_ENDL;
                    max_on_screen_vsize = llmax(max_on_screen_vsize, vsize);
                    max_vsize = llmax(max_vsize, vsize * bias);
                    // </FS:minerjr> [FIRE-35081]
                }
            }

            // <FS> [FIRE-35081] Blurry prims not changing with graphics settings
            //if (max_vsize >= LLViewerFetchedTexture::sMaxVirtualSize
            //    && (on_screen || LLViewerTexture::sDesiredDiscardBias <= BIAS_TRS_ON_SCREEN))
            //{
            //    break;
            //}
            // </FS>
        }

        // <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings
        // Replaced all the checks for this bool to be only in this 1 place instead of in the loop.
        // If the on screen counter is greater then 0, then there was at least 1 on screen texture
        on_screen = bool(on_screen_count);
        imagep->setCloseToCamera(close_to_camera > 0.0f ? 1.0f : 0.0f);

        //if (face_count > 1024)
        // Add check for if the image is animated to boost to high as well
        if (face_count > 1024 || animated != 0)
        // </FS:minerjr> [FIRE-35081]
        { // this texture is used in so many places we should just boost it and not bother checking its vsize
            // this is especially important because the above is not time sliced and can hit multiple ms for a single texture
            imagep->setBoostLevel(LLViewerFetchedTexture::BOOST_HIGH);
            // Do we ever remove it? This also sets texture nodelete!
        }

        if (imagep->getType() == LLViewerTexture::LOD_TEXTURE && imagep->getBoostLevel() == LLViewerTexture::BOOST_NONE)
        { // conditionally reset max virtual size for unboosted LOD_TEXTURES
          // this is an alternative to decaying mMaxVirtualSize over time
          // that keeps textures from continously downrezzing and uprezzing in the background

            if (LLViewerTexture::sDesiredDiscardBias > BIAS_TRS_OUT_OF_SCREEN ||
                (!on_screen && LLViewerTexture::sDesiredDiscardBias > BIAS_TRS_ON_SCREEN))
            {
                imagep->mMaxVirtualSize = 0.f;
            }
        }

        // <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings
        //imagep->addTextureStats(max_vsize);
        // New logic block for the bias system        
        // Then depending on the type of texture, the higher resolution on_screen_max_vsize is applied.
        // On Screen (Without Bias applied:
        //      LOD/Fetch Texture: Discard Levels 0, 1
        //      Fetch Texture 2, 3, 5 with bias < 2.0
        //      BoostLevel = Boost_High
        //      Local, Media, Dynamic Texture        
        // If the textures are on screen and either 1 are the first 2 levels of discard and are either fetched or LOD textures
        if (on_screen && ((imagep->getDiscardLevel() < 2 && imagep->getType() >= LLViewerTexture::FETCHED_TEXTURE) || (imagep->getType() == LLViewerTexture::FETCHED_TEXTURE && LLViewerTexture::sDesiredDiscardBias < 2.0f)))
        {
            // Always use the best quality of the texture
            imagep->addTextureStats(max_on_screen_vsize);
        }
        // If the boost level just became high, or the texture is (Local, Media Dynamic)
        else if (imagep->getBoostLevel() >= LLViewerTexture::BOOST_HIGH || imagep->getType() < LLViewerTexture::FETCHED_TEXTURE || close_to_camera)
        {
            // Always use the best quality of the texture
            imagep->addTextureStats(max_on_screen_vsize);
        }
        // All other texture cases will use max_vsize with bias applied.
        else
        {
            imagep->addTextureStats(max_vsize);
        }
        // </FS:minerjr> [FIRE-35081]
    }

#if 0
    imagep->setDebugText(llformat("%d/%d - %d/%d -- %d/%d",
        (S32)sqrtf(max_vsize),
        (S32)sqrtf(imagep->mMaxVirtualSize),
        imagep->getDiscardLevel(),
        imagep->getDesiredDiscardLevel(),
        imagep->getWidth(),
        imagep->getFullWidth()));
#endif

    // make sure to addTextureStats for any spotlights that are using this texture
    for (S32 vi = 0; vi < imagep->getNumVolumes(LLRender::LIGHT_TEX); ++vi)
    {
        LLVOVolume* volume = (*imagep->getVolumeList(LLRender::LIGHT_TEX))[vi];
        volume->updateSpotLightPriority();
    }

    F32 max_inactive_time = 20.f; // inactive time before deleting saved raw image
    S32 min_refs = 3; // 1 for mImageList, 1 for mUUIDMap, and 1 for "entries" in updateImagesFetchTextures

    F32 lazy_flush_timeout = 30.f; // delete unused images after 30 seconds

    //
    // Flush formatted images using a lazy flush
    //
    S32 num_refs = imagep->getNumRefs();
    if (num_refs <= min_refs && flush_images)
    {
        if (imagep->getLastReferencedTimer()->getElapsedTimeF32() > lazy_flush_timeout)
        {
            // Remove the unused image from the image list
            deleteImage(imagep);
            return;
        }
    }
    else
    {
        // still referenced outside of image list, reset timer
        imagep->getLastReferencedTimer()->reset();

        if (imagep->hasSavedRawImage())
        {
            if (imagep->getElapsedLastReferencedSavedRawImageTime() > max_inactive_time)
            {
                imagep->destroySavedRawImage();
            }
        }

        if (imagep->isDeleted())
        {
            return;
        }
    }

    if (!imagep->isInImageList())
    {
        return;
    }
    if (imagep->isInFastCacheList())
    {
        return; //wait for loading from the fast cache.
    }

    imagep->processTextureStats();
}

F32 LLViewerTextureList::updateImagesCreateTextures(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (gGLManager.mIsDisabled) return 0.0f;

    //
    // Create GL textures for all textures that need them (images which have been
    // decoded, but haven't been pushed into GL).
    //

    LLTimer create_timer;

    while (!mCreateTextureList.empty())
    {
        LLViewerFetchedTexture* imagep = mCreateTextureList.front();
        llassert(imagep->mCreatePending);

        // desired discard may change while an image is being decoded. If the texture in VRAM is sufficient
        // for the current desired discard level, skip the texture creation.  This happens more often than it probably
        // should
        bool redundant_load = imagep->hasGLTexture() && imagep->getDiscardLevel() <= imagep->getDesiredDiscardLevel();

        if (!redundant_load)
        {
           imagep->createTexture();
        }

        imagep->postCreateTexture();
        imagep->mCreatePending = false;
        mCreateTextureList.pop();

        if (imagep->hasGLTexture() && imagep->getDiscardLevel() < imagep->getDesiredDiscardLevel() &&
           // <FS:minerjr> [FIRE-35081] Blurry prims not changing with graphics settings
           //(imagep->getDesiredDiscardLevel() <= MAX_DISCARD_LEVEL))
           // Add additional restrictions on scaling down (only BOOST_NONE LOD Textures (Also skip media)
           (imagep->getDesiredDiscardLevel() <= MAX_DISCARD_LEVEL) && imagep->getBoostLevel() == LLViewerTexture::BOOST_NONE && imagep->getType() == LLViewerTexture::LOD_TEXTURE && !imagep->hasParcelMedia() && !imagep->isViewerMediaTexture())
           // </FS:minerjr> [FIRE-35081]
        {
            // NOTE: this may happen if the desired discard reduces while a decode is in progress and does not
            // necessarily indicate a problem, but if log occurrences excede that of dsiplay_stats: FPS,
            // something has probably gone wrong.
            LL_WARNS_ONCE("Texture") << "Texture will be downscaled immediately after loading." << LL_ENDL;
            imagep->scaleDown();
        }

        if (create_timer.getElapsedTimeF32() > max_time * 0.5f)
        {
            break;
        }
    }

    if (!mDownScaleQueue.empty() && gPipeline.mDownResMap.isComplete())
    {
        LLGLDisable blend(GL_BLEND);
        gGL.setColorMask(true, true);

        // just in case we downres textures, bind downresmap and copy program
        gPipeline.mDownResMap.bindTarget();
        gCopyProgram.bind();
        gPipeline.mScreenTriangleVB->setBuffer();

        // give time to downscaling first -- if mDownScaleQueue is not empty, we're running out of memory and need
        // to free up memory by discarding off screen textures quickly

        // do at least 5 and make sure we don't get too far behind even if it violates
        // the time limit.  If we don't downscale quickly the viewer will hit swap and may
        // freeze.
        S32 min_count = (S32)mCreateTextureList.size() / 20 + 5;

        //create_timer.reset();
        while (!mDownScaleQueue.empty())
        {
            LLViewerFetchedTexture* image = mDownScaleQueue.front();
            llassert(image->mDownScalePending);

            LLImageGL* img = image->getGLTexture();
            if (img && img->getHasGLTexture())
            {
                img->scaleDown(image->getDesiredDiscardLevel());
            }

            image->mDownScalePending = false;
            mDownScaleQueue.pop();

            if (create_timer.getElapsedTimeF32() > max_time && --min_count <= 0)
            {
                break;
            }
        }

        gCopyProgram.unbind();
        gPipeline.mDownResMap.flush();
    }

    return create_timer.getElapsedTimeF32();
}

F32 LLViewerTextureList::updateImagesLoadingFastCache(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (gGLManager.mIsDisabled) return 0.0f;
    if(mFastCacheList.empty())
    {
        return 0.f;
    }

    //
    // loading texture raw data from the fast cache directly.
    //

    LLTimer timer;
    image_list_t::iterator enditer = mFastCacheList.begin();
    for (image_list_t::iterator iter = mFastCacheList.begin();
         iter != mFastCacheList.end();)
    {
        image_list_t::iterator curiter = iter++;
        enditer = iter;
        LLViewerFetchedTexture *imagep = *curiter;
        imagep->loadFromFastCache();
        // <FS:Ansariel> Fast cache stats
        sNumFastCacheReads++;
        // </FS:Ansariel>
        // <FS:Ansariel> Fix fast cache
        if (timer.getElapsedTimeF32() > max_time)
            break;
        // </FS:Ansariel>
    }
    mFastCacheList.erase(mFastCacheList.begin(), enditer);
    return timer.getElapsedTimeF32();
}

void LLViewerTextureList::forceImmediateUpdate(LLViewerFetchedTexture* imagep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!imagep || gCubeSnapshot)
    {
        return ;
    }

    imagep->processTextureStats();

    return ;
}

F32 LLViewerTextureList::updateImagesFetchTextures(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    typedef std::vector<LLPointer<LLViewerFetchedTexture> > entries_list_t;
    entries_list_t entries;

    // update N textures at beginning of mImageList
    U32 update_count = 0;
    static const S32 MIN_UPDATE_COUNT = gSavedSettings.getS32("TextureFetchUpdateMinCount");       // default: 32

    // NOTE:  a texture may be deleted as a side effect of some of these updates
    // Deletion rules check ref count, so be careful not to hold any LLPointer references to the textures here other than the one in entries.

    //update MIN_UPDATE_COUNT or 5% of other textures, whichever is greater
    update_count = llmax((U32) MIN_UPDATE_COUNT, (U32) mUUIDMap.size()/20);
    if (LLViewerTexture::sDesiredDiscardBias > 1.f
        && LLViewerTexture::sBiasTexturesUpdated < (U32)mUUIDMap.size())
    {
        // We are over memory target. Bias affects discard rates, so update
        // existing textures agresively to free memory faster.
        update_count = (S32)(update_count * LLViewerTexture::sDesiredDiscardBias);

        // This isn't particularly precise and can overshoot, but it doesn't need
        // to be, just making sure it did a full circle and doesn't get stuck updating
        // at bias = 4 with 4 times the rate permanently.
        LLViewerTexture::sBiasTexturesUpdated += update_count;
    }
    update_count = llmin(update_count, (U32) mUUIDMap.size());

    { // copy entries out of UUID map to avoid iterator invalidation from deletion inside updateImageDecodeProiroty or updateFetch below
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vtluift - copy");

        // copy entries out of UUID map for updating
        entries.reserve(update_count);
        uuid_map_t::iterator iter = mUUIDMap.upper_bound(mLastUpdateKey);
        while (update_count-- > 0)
        {
            if (iter == mUUIDMap.end())
            {
                iter = mUUIDMap.begin();
            }

            if (iter->second->getGLTexture())
            {
                entries.push_back(iter->second);
            }
            ++iter;
        }
    }

    LLTimer timer;

    for (auto& imagep : entries)
    {
        mLastUpdateKey = LLTextureKey(imagep->getID(), (ETexListType)imagep->getTextureListType());

        if (imagep->getNumRefs() > 1) // make sure this image hasn't been deleted before attempting to update (may happen as a side effect of some other image updating)
        {
            updateImageDecodePriority(imagep);
            imagep->updateFetch();
        }

        if (timer.getElapsedTimeF32() > max_time)
        {
            break;
        }
    }

    return timer.getElapsedTimeF32();
}

void LLViewerTextureList::updateImagesUpdateStats()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mForceResetTextureStats)
    {
        for (image_list_t::iterator iter = mImageList.begin();
             iter != mImageList.end(); )
        {
            LLViewerFetchedTexture* imagep = *iter++;
            imagep->resetTextureStats();
        }
        mForceResetTextureStats = false;
    }
}

void LLViewerTextureList::decodeAllImages(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTimer timer;

    //loading from fast cache
    // <FS:Ansariel> Fix fast cache
    //updateImagesLoadingFastCache(max_time);
    max_time -= updateImagesLoadingFastCache(max_time);

    // Update texture stats and priorities
    std::vector<LLPointer<LLViewerFetchedTexture> > image_list;
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); )
    {
        LLViewerFetchedTexture* imagep = *iter++;
        image_list.push_back(imagep);
        imagep->setInImageList(false) ;
    }

    llassert_always(image_list.size() == mImageList.size()) ;
    mImageList.clear();
    for (std::vector<LLPointer<LLViewerFetchedTexture> >::iterator iter = image_list.begin();
         iter != image_list.end(); ++iter)
    {
        LLViewerFetchedTexture* imagep = *iter;
        imagep->processTextureStats();
        addImageToList(imagep);
    }
    image_list.clear();

    // Update fetch (decode)
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); )
    {
        LLViewerFetchedTexture* imagep = *iter++;
        imagep->updateFetch();
    }
    std::shared_ptr<LL::WorkQueue> main_queue = LLImageGLThread::sEnabledTextures ? LL::WorkQueue::getInstance("mainloop") : NULL;
    // Run threads
    size_t fetch_pending = 0;
    while (1)
    {
        LLAppViewer::instance()->getTextureCache()->update(1); // unpauses the texture cache thread
        LLAppViewer::instance()->getImageDecodeThread()->update(1); // unpauses the image thread
        fetch_pending = LLAppViewer::instance()->getTextureFetch()->update(1); // unpauses the texture fetch thread

        if (LLImageGLThread::sEnabledTextures)
        {
            main_queue->runFor(std::chrono::milliseconds(1));
            fetch_pending += main_queue->size();
        }

        if (fetch_pending == 0 || timer.getElapsedTimeF32() > max_time)
        {
            break;
        }
    }
    // Update fetch again
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); )
    {
        LLViewerFetchedTexture* imagep = *iter++;
        imagep->updateFetch();
    }
    max_time -= timer.getElapsedTimeF32();
    max_time = llmax(max_time, .001f);
    F32 create_time = updateImagesCreateTextures(max_time);

    LL_DEBUGS("ViewerImages") << "decodeAllImages() took " << timer.getElapsedTimeF32() << " seconds. "
    << " fetch_pending " << fetch_pending
    << " create_time " << create_time
    << LL_ENDL;
}

bool LLViewerTextureList::createUploadFile(LLPointer<LLImageRaw> raw_image,
                                           const std::string& out_filename,
                                           const S32 max_image_dimentions,
                                           const S32 min_image_dimentions)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    LLImageDataSharedLock lock(raw_image);

    // make a copy, since convertToUploadFile scales raw image
    LLPointer<LLImageRaw> scale_image = new LLImageRaw(
        raw_image->getData(),
        raw_image->getWidth(),
        raw_image->getHeight(),
        raw_image->getComponents());

// <AS:Chanayane> Lossless OpenJPEG uploads
    /*
    LLPointer<LLImageJ2C> compressedImage = LLViewerTextureList::convertToUploadFile(scale_image, max_image_dimentions);
    */
    LLPointer<LLImageJ2C> compressedImage = LLViewerTextureList::convertToUploadFile(scale_image, max_image_dimentions, false, true);
// <AS:Chanayane>

    if (compressedImage->getWidth() < min_image_dimentions || compressedImage->getHeight() < min_image_dimentions)
    {
        std::string reason = llformat("Images below %d x %d pixels are not allowed. Actual size: %d x %dpx",
                                      min_image_dimentions,
                                      min_image_dimentions,
                                      compressedImage->getWidth(),
                                      compressedImage->getHeight());
        compressedImage->setLastError(reason);
        return false;
    }
    if (compressedImage.isNull())
    {
        compressedImage->setLastError("Couldn't convert the image to jpeg2000.");
        LL_INFOS() << "Couldn't convert to j2c, file : " << out_filename << LL_ENDL;
        return false;
    }
    if (!compressedImage->save(out_filename))
    {
        compressedImage->setLastError("Couldn't create the jpeg2000 image for upload.");
        LL_INFOS() << "Couldn't create output file : " << out_filename << LL_ENDL;
        return false;
    }
    return true;
}

bool LLViewerTextureList::createUploadFile(const std::string& filename,
                                         const std::string& out_filename,
                                         const U8 codec,
                                         const S32 max_image_dimentions,
                                         const S32 min_image_dimentions,
                                         bool force_square)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    try
    {
    // Load the image
    LLPointer<LLImageFormatted> image = LLImageFormatted::createFromType(codec);
    if (image.isNull())
    {
        LL_WARNS() << "Couldn't open the image to be uploaded." << LL_ENDL;
        return false;
    }
    if (!image->load(filename))
    {
        image->setLastError("Couldn't load the image to be uploaded.");
        return false;
    }

    // calcDataSizeJ2C assumes maximum size is 2048 and for bigger images can
    // assign discard to bring imige to needed size, but upload does the scaling
    // as needed, so just reset discard.
    // Assume file is full and has 'discard' 0 data.
    // Todo: probably a better idea to have some setMaxDimentions in J2C
    // called when loading from a local file
    image->setDiscardLevel(0);

    // Decompress or expand it in a raw image structure
    LLPointer<LLImageRaw> raw_image = new LLImageRaw;
    if (!image->decode(raw_image, 0.0f))
    {
        image->setLastError("Couldn't decode the image to be uploaded.");
        return false;
    }
    // Check the image constraints
    if ((image->getComponents() != 3) && (image->getComponents() != 4))
    {
        image->setLastError("Image files with less than 3 or more than 4 components are not supported.");
        return false;
    }
    if (image->getWidth() < min_image_dimentions || image->getHeight() < min_image_dimentions)
    {
        std::string reason = llformat("Images below %d x %d pixels are not allowed. Actual size: %d x %dpx",
            min_image_dimentions,
            min_image_dimentions,
            image->getWidth(),
            image->getHeight());
        image->setLastError(reason);
        return false;
    }
    // Convert to j2c (JPEG2000) and save the file locally
// <AS:Chanayane> Lossless OpenJPEG uploads
    /*
    LLPointer<LLImageJ2C> compressedImage = convertToUploadFile(raw_image, max_image_dimentions, force_square);
    */
    LLPointer<LLImageJ2C> compressedImage = convertToUploadFile(raw_image, max_image_dimentions, force_square, true);
// </AS:Chanayane>
    if (compressedImage.isNull())
    {
        image->setLastError("Couldn't convert the image to jpeg2000.");
        LL_INFOS() << "Couldn't convert to j2c, file : " << filename << LL_ENDL;
        return false;
    }
    if (!compressedImage->save(out_filename))
    {
        image->setLastError("Couldn't create the jpeg2000 image for upload.");
        LL_INFOS() << "Couldn't create output file : " << out_filename << LL_ENDL;
        return false;
    }
    // Test to see if the encode and save worked
    LLPointer<LLImageJ2C> integrity_test = new LLImageJ2C;
    if (!integrity_test->loadAndValidate( out_filename ))
    {
        image->setLastError("The created jpeg2000 image is corrupt.");
        LL_INFOS() << "Image file : " << out_filename << " is corrupt" << LL_ENDL;
        return false;
    }
    }
    catch (...)
    {
        LOG_UNHANDLED_EXCEPTION("");
        return false;
    }
    return true;
}

// note: modifies the argument raw_image!!!!
LLPointer<LLImageJ2C> LLViewerTextureList::convertToUploadFile(LLPointer<LLImageRaw> raw_image, const S32 max_image_dimentions, bool force_square, bool force_lossless)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLImageDataLock lock(raw_image);

    if (force_square)
    {
        S32 biggest_side = llmax(raw_image->getWidth(), raw_image->getHeight());
        S32 square_size = raw_image->biasedDimToPowerOfTwo(biggest_side, max_image_dimentions);

        raw_image->scale(square_size, square_size);
    }
    else
    {
        raw_image->biasedScaleToPowerOfTwo(max_image_dimentions);
    }
    LLPointer<LLImageJ2C> compressedImage = new LLImageJ2C();

    if (force_lossless ||
        (gSavedSettings.getBOOL("LosslessJ2CUpload") &&
            (raw_image->getWidth() * raw_image->getHeight() <= LL_IMAGE_REZ_LOSSLESS_CUTOFF * LL_IMAGE_REZ_LOSSLESS_CUTOFF)))
    {
        compressedImage->setReversible(true);
    }


    if (gSavedSettings.getBOOL("Jpeg2000AdvancedCompression"))
    {
        // This test option will create jpeg2000 images with precincts for each level, RPCL ordering
        // and PLT markers. The block size is also optionally modifiable.
        // Note: the images hence created are compatible with older versions of the viewer.
        // Read the blocks and precincts size settings
        S32 block_size = gSavedSettings.getS32("Jpeg2000BlocksSize");
        S32 precinct_size = gSavedSettings.getS32("Jpeg2000PrecinctsSize");
        LL_INFOS() << "Advanced JPEG2000 Compression: precinct = " << precinct_size << ", block = " << block_size << LL_ENDL;
        compressedImage->initEncode(*raw_image, block_size, precinct_size, 0);
    }

    if (!compressedImage->encode(raw_image, 0.0f))
    {
        LL_INFOS() << "convertToUploadFile : encode returns with error!!" << LL_ENDL;
        // Clear up the pointer so we don't leak that one
        compressedImage = NULL;
    }

    return compressedImage;
}

///////////////////////////////////////////////////////////////////////////////

// <FS:Ansariel> OpenSim compatibility
// static
void LLViewerTextureList::receiveImageHeader(LLMessageSystem *msg, void **user_data)
{
    static LLCachedControl<bool> log_texture_traffic(gSavedSettings,"LogTextureNetworkTraffic", false) ;

    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    // Receive image header, copy into image object and decompresses
    // if this is a one-packet image.

    LLUUID id;

    char ip_string[256];
    u32_to_ip_string(msg->getSenderIP(),ip_string);

    U32Bytes received_size ;
    if (msg->getReceiveCompressedSize())
    {
        received_size = (U32Bytes)msg->getReceiveCompressedSize() ;
    }
    else
    {
        received_size = (U32Bytes)msg->getReceiveSize() ;
    }
    add(LLStatViewer::TEXTURE_NETWORK_DATA_RECEIVED, received_size);
    add(LLStatViewer::TEXTURE_PACKETS, 1);

    U8 codec;
    U16 packets;
    U32 totalbytes;
    msg->getUUIDFast(_PREHASH_ImageID, _PREHASH_ID, id);
    msg->getU8Fast(_PREHASH_ImageID, _PREHASH_Codec, codec);
    msg->getU16Fast(_PREHASH_ImageID, _PREHASH_Packets, packets);
    msg->getU32Fast(_PREHASH_ImageID, _PREHASH_Size, totalbytes);

    S32 data_size = msg->getSizeFast(_PREHASH_ImageData, _PREHASH_Data);
    if (!data_size)
    {
        return;
    }
    if (data_size < 0)
    {
        // msg->getSizeFast() is probably trying to tell us there
        // was an error.
        LL_ERRS() << "image header chunk size was negative: "
        << data_size << LL_ENDL;
        return;
    }

    // this buffer gets saved off in the packet list
    U8 *data = new U8[data_size];
    msg->getBinaryDataFast(_PREHASH_ImageData, _PREHASH_Data, data, data_size);

    LLViewerFetchedTexture *image = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
    if (!image)
    {
        delete [] data;
        return;
    }
    if(log_texture_traffic)
    {
        gTotalTextureBytesPerBoostLevel[image->getBoostLevel()] += received_size ;
    }

    //image->getLastPacketTimer()->reset();
    bool res = LLAppViewer::getTextureFetch()->receiveImageHeader(msg->getSender(), id, codec, packets, totalbytes, data_size, data);
    if (!res)
    {
        delete[] data;
    }
}

// static
void LLViewerTextureList::receiveImagePacket(LLMessageSystem *msg, void **user_data)
{
    static LLCachedControl<bool> log_texture_traffic(gSavedSettings,"LogTextureNetworkTraffic", false) ;

    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    // Receives image packet, copy into image object,
    // checks if all packets received, decompresses if so.

    LLUUID id;
    U16 packet_num;

    char ip_string[256];
    u32_to_ip_string(msg->getSenderIP(),ip_string);

    U32Bytes received_size ;
    if (msg->getReceiveCompressedSize())
    {
        received_size = (U32Bytes)msg->getReceiveCompressedSize() ;
    }
    else
    {
        received_size = (U32Bytes)msg->getReceiveSize() ;
    }

    add(LLStatViewer::TEXTURE_NETWORK_DATA_RECEIVED, F64Bytes(received_size));
    add(LLStatViewer::TEXTURE_PACKETS, 1);

    //llprintline("Start decode, image header...");
    msg->getUUIDFast(_PREHASH_ImageID, _PREHASH_ID, id);
    msg->getU16Fast(_PREHASH_ImageID, _PREHASH_Packet, packet_num);
    S32 data_size = msg->getSizeFast(_PREHASH_ImageData, _PREHASH_Data);

    if (!data_size)
    {
        return;
    }
    if (data_size < 0)
    {
        // msg->getSizeFast() is probably trying to tell us there
        // was an error.
        LL_ERRS() << "image data chunk size was negative: "
        << data_size << LL_ENDL;
        return;
    }
    if (data_size > MTUBYTES)
    {
        LL_ERRS() << "image data chunk too large: " << data_size << " bytes" << LL_ENDL;
        return;
    }
    U8 *data = new U8[data_size];
    msg->getBinaryDataFast(_PREHASH_ImageData, _PREHASH_Data, data, data_size);

    LLViewerFetchedTexture *image = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
    if (!image)
    {
        delete [] data;
        return;
    }
    if(log_texture_traffic)
    {
        gTotalTextureBytesPerBoostLevel[image->getBoostLevel()] += received_size ;
    }

    //image->getLastPacketTimer()->reset();
    bool res = LLAppViewer::getTextureFetch()->receiveImagePacket(msg->getSender(), id, packet_num, data_size, data);
    if (!res)
    {
        delete[] data;
    }
}
// </FS:Ansariel>

// We've been that the asset server does not contain the requested image id.
// static
void LLViewerTextureList::processImageNotInDatabase(LLMessageSystem *msg,void **user_data)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLUUID image_id;
    msg->getUUIDFast(_PREHASH_ImageID, _PREHASH_ID, image_id);

    LLViewerFetchedTexture* image = gTextureList.findImage( image_id, TEX_LIST_STANDARD);
    if( image )
    {
        LL_WARNS() << "Image not in db" << LL_ENDL;
        image->setIsMissingAsset();
    }

    image = gTextureList.findImage(image_id, TEX_LIST_SCALE);
    if (image)
    {
        LL_WARNS() << "Icon not in db" << LL_ENDL;
        image->setIsMissingAsset();
    }
}


///////////////////////////////////////////////////////////////////////////////

// explicitly cleanup resources, as this is a singleton class with process
// lifetime so ability to perform std::map operations in destructor is not
// guaranteed.
void LLUIImageList::cleanUp()
{
    mUIImages.clear();
    mUITextureList.clear() ;
}

LLUIImagePtr LLUIImageList::getUIImageByID(const LLUUID& image_id, S32 priority)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // use id as image name
    std::string image_name = image_id.asString();

    // look for existing image
    uuid_ui_image_map_t::iterator found_it = mUIImages.find(image_name);
    if (found_it != mUIImages.end())
    {
        return found_it->second;
    }

    const bool use_mips = false;
    const LLRect scale_rect = LLRect::null;
    const LLRect clip_rect = LLRect::null;
    return loadUIImageByID(image_id, use_mips, scale_rect, clip_rect, (LLViewerTexture::EBoostLevel)priority);
}

LLUIImagePtr LLUIImageList::getUIImage(const std::string& image_name, S32 priority)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // look for existing image
    uuid_ui_image_map_t::iterator found_it = mUIImages.find(image_name);
    if (found_it != mUIImages.end())
    {
        return found_it->second;
    }

    const bool use_mips = false;
    const LLRect scale_rect = LLRect::null;
    const LLRect clip_rect = LLRect::null;
    return loadUIImageByName(image_name, image_name, use_mips, scale_rect, clip_rect, (LLViewerTexture::EBoostLevel)priority);
}

LLUIImagePtr LLUIImageList::loadUIImageByName(const std::string& name, const std::string& filename,
                                              bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect, LLViewerTexture::EBoostLevel boost_priority,
                                              LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (boost_priority == LLGLTexture::BOOST_NONE)
    {
        boost_priority = LLGLTexture::BOOST_UI;
    }
    LLViewerFetchedTexture* imagep = LLViewerTextureManager::getFetchedTextureFromFile(filename, FTT_LOCAL_FILE, MIPMAP_NO, boost_priority);
    return loadUIImage(imagep, name, use_mips, scale_rect, clip_rect, scale_style);
}

LLUIImagePtr LLUIImageList::loadUIImageByID(const LLUUID& id,
                                            bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect, LLViewerTexture::EBoostLevel boost_priority,
                                            LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (boost_priority == LLGLTexture::BOOST_NONE)
    {
        boost_priority = LLGLTexture::BOOST_UI;
    }
    LLViewerFetchedTexture* imagep = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, MIPMAP_NO, boost_priority);
    return loadUIImage(imagep, id.asString(), use_mips, scale_rect, clip_rect, scale_style);
}

LLUIImagePtr LLUIImageList::loadUIImage(LLViewerFetchedTexture* imagep, const std::string& name, bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect,
                                        LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!imagep) return NULL;

    imagep->setAddressMode(LLTexUnit::TAM_CLAMP);

    //don't compress UI images
    imagep->getGLTexture()->setAllowCompression(false);

    LLUIImagePtr new_imagep = new LLUIImage(name, imagep);
    new_imagep->setScaleStyle(scale_style);

    if (imagep->getBoostLevel() != LLGLTexture::BOOST_ICON
        && imagep->getBoostLevel() != LLGLTexture::BOOST_THUMBNAIL
        && imagep->getBoostLevel() != LLGLTexture::BOOST_PREVIEW)
    {
        // Don't add downloadable content into this list
        // all UI images are non-deletable and list does not support deletion
        imagep->setNoDelete();
        mUIImages.insert(std::make_pair(name, new_imagep));
        mUITextureList.push_back(imagep);
    }

    //Note:
    //Some other textures such as ICON also through this flow to be fetched.
    //But only UI textures need to set this callback.
    if(imagep->getBoostLevel() == LLGLTexture::BOOST_UI)
    {
        LLUIImageLoadData* datap = new LLUIImageLoadData;
        datap->mImageName = name;
        datap->mImageScaleRegion = scale_rect;
        datap->mImageClipRegion = clip_rect;

        imagep->setLoadedCallback(onUIImageLoaded, 0, false, false, datap, NULL);
    }
    return new_imagep;
}

LLUIImagePtr LLUIImageList::preloadUIImage(const std::string& name, const std::string& filename, bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect, LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // look for existing image
    uuid_ui_image_map_t::iterator found_it = mUIImages.find(name);
    if (found_it != mUIImages.end())
    {
        // image already loaded!
        LL_ERRS() << "UI Image " << name << " already loaded." << LL_ENDL;
    }

    return loadUIImageByName(name, filename, use_mips, scale_rect, clip_rect, LLGLTexture::BOOST_UI, scale_style);
}

//static
void LLUIImageList::onUIImageLoaded( bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* src_aux, S32 discard_level, bool final, void* user_data )
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!success || !user_data)
    {
        return;
    }

    LLUIImageLoadData* image_datap = (LLUIImageLoadData*)user_data;
    std::string ui_image_name = image_datap->mImageName;
    LLRect scale_rect = image_datap->mImageScaleRegion;
    LLRect clip_rect = image_datap->mImageClipRegion;
    if (final)
    {
        delete image_datap;
    }

    LLUIImageList* instance = getInstance();

    uuid_ui_image_map_t::iterator found_it = instance->mUIImages.find(ui_image_name);
    if (found_it != instance->mUIImages.end())
    {
        LLUIImagePtr imagep = found_it->second;

        // for images grabbed from local files, apply clipping rectangle to restore original dimensions
        // from power-of-2 gl image
        if (success && imagep.notNull() && src_vi && (src_vi->getUrl().compare(0, 7, "file://")==0))
        {
            F32 full_width = (F32)src_vi->getFullWidth();
            F32 full_height = (F32)src_vi->getFullHeight();
            F32 clip_x = (F32)src_vi->getOriginalWidth() / full_width;
            F32 clip_y = (F32)src_vi->getOriginalHeight() / full_height;
            if (clip_rect != LLRect::null)
            {
                imagep->setClipRegion(LLRectf(llclamp((F32)clip_rect.mLeft / full_width, 0.f, 1.f),
                                            llclamp((F32)clip_rect.mTop / full_height, 0.f, 1.f),
                                            llclamp((F32)clip_rect.mRight / full_width, 0.f, 1.f),
                                            llclamp((F32)clip_rect.mBottom / full_height, 0.f, 1.f)));
            }
            else
            {
                imagep->setClipRegion(LLRectf(0.f, clip_y, clip_x, 0.f));
            }
            if (scale_rect != LLRect::null)
            {
                imagep->setScaleRegion(
                    LLRectf(llclamp((F32)scale_rect.mLeft / (F32)imagep->getWidth(), 0.f, 1.f),
                        llclamp((F32)scale_rect.mTop / (F32)imagep->getHeight(), 0.f, 1.f),
                        llclamp((F32)scale_rect.mRight / (F32)imagep->getWidth(), 0.f, 1.f),
                        llclamp((F32)scale_rect.mBottom / (F32)imagep->getHeight(), 0.f, 1.f)));
            }

            imagep->onImageLoaded();
        }
    }
}

namespace LLInitParam
{
    template<>
    struct TypeValues<LLUIImage::EScaleStyle> : public TypeValuesHelper<LLUIImage::EScaleStyle>
    {
        static void declareValues()
        {
            declare("scale_inner",  LLUIImage::SCALE_INNER);
            declare("scale_outer",  LLUIImage::SCALE_OUTER);
        }
    };
}

struct UIImageDeclaration : public LLInitParam::Block<UIImageDeclaration>
{
    Mandatory<std::string>      name;
    Optional<std::string>       file_name;
    Optional<bool>              preload;
    Optional<LLRect>            scale;
    Optional<LLRect>            clip;
    Optional<bool>              use_mips;
    Optional<LLUIImage::EScaleStyle> scale_type;

    UIImageDeclaration()
    :   name("name"),
        file_name("file_name"),
        preload("preload", false),
        scale("scale"),
        clip("clip"),
        use_mips("use_mips", false),
        scale_type("scale_type", LLUIImage::SCALE_INNER)
    {}
};

struct UIImageDeclarations : public LLInitParam::Block<UIImageDeclarations>
{
    Mandatory<S32>  version;
    Multiple<UIImageDeclaration> textures;

    UIImageDeclarations()
    :   version("version"),
        textures("texture")
    {}
};

bool LLUIImageList::initFromFile()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // Look for textures.xml in all the right places. Pass
    // constraint=LLDir::ALL_SKINS because we want to overlay textures.xml
    // from all the skins directories.
    std::vector<std::string> textures_paths =
        gDirUtilp->findSkinnedFilenames(LLDir::TEXTURES, "textures.xml", LLDir::ALL_SKINS);
    std::vector<std::string>::const_iterator pi(textures_paths.begin()), pend(textures_paths.end());
    if (pi == pend)
    {
        LL_WARNS() << "No textures.xml found in skins directories" << LL_ENDL;
        return false;
    }

    // The first (most generic) file gets special validations
    LLXMLNodePtr root;
    if (!LLXMLNode::parseFile(*pi, root, NULL))
    {
        LL_WARNS() << "Unable to parse UI image list file " << *pi << LL_ENDL;
        return false;
    }
    if (!root->hasAttribute("version"))
    {
        LL_WARNS() << "No valid version number in UI image list file " << *pi << LL_ENDL;
        return false;
    }

    UIImageDeclarations images;
    LLXUIParser parser;
    parser.readXUI(root, images, *pi);

    // add components defined in the rest of the skin paths
    while (++pi != pend)
    {
        LLXMLNodePtr update_root;
        if (LLXMLNode::parseFile(*pi, update_root, NULL))
        {
            parser.readXUI(update_root, images, *pi);
        }
    }

    if (!images.validateBlock()) return false;

    std::map<std::string, UIImageDeclaration> merged_declarations;
    for (LLInitParam::ParamIterator<UIImageDeclaration>::const_iterator image_it = images.textures.begin();
        image_it != images.textures.end();
        ++image_it)
    {
        merged_declarations[image_it->name].overwriteFrom(*image_it);
    }

    enum e_decode_pass
    {
        PASS_DECODE_NOW,
        PASS_DECODE_LATER,
        NUM_PASSES
    };

    for (S32 cur_pass = PASS_DECODE_NOW; cur_pass < NUM_PASSES; cur_pass++)
    {
        for (std::map<std::string, UIImageDeclaration>::const_iterator image_it = merged_declarations.begin();
            image_it != merged_declarations.end();
            ++image_it)
        {
            const UIImageDeclaration& image = image_it->second;
            std::string file_name = image.file_name.isProvided() ? image.file_name() : image.name();

            // load high priority textures on first pass (to kick off decode)
            enum e_decode_pass decode_pass = image.preload ? PASS_DECODE_NOW : PASS_DECODE_LATER;
            if (decode_pass != cur_pass)
            {
                continue;
            }
            preloadUIImage(image.name, file_name, image.use_mips, image.scale, image.clip, image.scale_type);
        }

        if (!gSavedSettings.getBOOL("NoPreload"))
        {
            if (cur_pass == PASS_DECODE_NOW)
            {
                // init fetching and decoding of preloaded images
                gTextureList.decodeAllImages(9.f);
            }
            else
            {
                // decodeAllImages needs two passes to refresh stats and priorities on second pass
                gTextureList.decodeAllImages(1.f);
            }
        }
    }
    return true;
}


