/**
 * @file llinventoryfunctions.h
 * @brief Miscellaneous inventory-related functions and classes
 * class definition
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#ifndef LL_LLINVENTORYFUNCTIONS_H
#define LL_LLINVENTORYFUNCTIONS_H

#include "llinventorymodel.h"
#include "llinventory.h"
#include "llhandle.h"
#include "llwearabletype.h"

// compute_stock_count() return error code
const S32 COMPUTE_STOCK_INFINITE = -1;
const S32 COMPUTE_STOCK_NOT_EVALUATED = -2;

// <FS:TT> - Firestorm folder name for use by AO, bridge and possibly others
#define ROOT_FIRESTORM_FOLDER   "#Firestorm"
// </FS:TT>

/********************************************************************************
 **                                                                            **
 **                    MISCELLANEOUS GLOBAL FUNCTIONS
 **/

// Is this a parent folder to a worn item
bool get_is_parent_to_worn_item(const LLUUID& id);

// Is this item or its baseitem is worn, attached, etc...
bool get_is_item_worn(const LLUUID& id);
bool get_is_item_worn(const LLViewerInventoryItem* item);

// Could this item be worn (correct type + not already being worn)
bool get_can_item_be_worn(const LLUUID& id);

bool get_is_item_removable(const LLInventoryModel* model, const LLUUID& id, bool check_worn);

// Performs the appropiate edit action (if one exists) for this item
bool get_is_item_editable(const LLUUID& inv_item_id);
void handle_item_edit(const LLUUID& inv_item_id);

bool get_is_category_removable(const LLInventoryModel* model, const LLUUID& id);
bool get_is_category_and_children_removable(LLInventoryModel* model, const LLUUID& folder_id, bool check_worn);

bool get_is_category_renameable(const LLInventoryModel* model, const LLUUID& id);

void show_item_profile(const LLUUID& item_uuid);
void show_task_item_profile(const LLUUID& item_uuid, const LLUUID& object_id);

void show_item_original(const LLUUID& item_uuid);
void reset_inventory_filter();

// <AS:Chanayane> Replace Links context menu entry
void replace_links(const LLUUID& item_uuid);
// </AS:Chanayane>

// <AS:Chanayane> Delete from outfit context menu entry
void delete_from_outfit(const uuid_vec_t& ids);
// </AS:Chanayane>

// Nudge the listing categories in the inventory to signal that their marketplace status changed
void update_marketplace_category(const LLUUID& cat_id, bool perform_consistency_enforcement = true, bool skip_clear_listing = false);
// Nudge all listing categories to signal that their marketplace status changed
void update_all_marketplace_count();

// [RLVa:KB] - Checked: RLVa-2.3 (Give-to-#RLV)
void rename_category(LLInventoryModel* model, const LLUUID& cat_id, const std::string& new_name, LLPointer<LLInventoryCallback> cb = nullptr);
// [/RLVa:KB]
//void rename_category(LLInventoryModel* model, const LLUUID& cat_id, const std::string& new_name);

void copy_inventory_category(LLInventoryModel* model, LLViewerInventoryCategory* cat, const LLUUID& parent_id, const LLUUID& root_copy_id = LLUUID::null, bool move_no_copy_items = false);
void copy_inventory_category(LLInventoryModel* model, LLViewerInventoryCategory* cat, const LLUUID& parent_id, const LLUUID& root_copy_id, bool move_no_copy_items, inventory_func_type callback);
void copy_inventory_category(LLInventoryModel* model, LLViewerInventoryCategory* cat, const LLUUID& parent_id, const LLUUID& root_copy_id, bool move_no_copy_items, LLPointer<LLInventoryCallback> callback);

void copy_inventory_category_content(const LLUUID& new_cat_uuid, LLInventoryModel* model, LLViewerInventoryCategory* cat, const LLUUID& root_copy_id, bool move_no_copy_items);

// Generates a string containing the path to the object specified by id (not including the object name).
void append_path(const LLUUID& id, std::string& path);

// Generates a string containing the path name of the object.
std::string make_path(const LLInventoryObject* object);
// Generates a string containing the path name of the object specified by id.
std::string make_inventory_path(const LLUUID& id);

// Generates a string containing the path name and id of the object.
std::string make_info(const LLInventoryObject* object);
// Generates a string containing the path name and id of the object specified by id.
std::string make_inventory_info(const LLUUID& id);

bool can_move_item_to_marketplace(const LLInventoryCategory* root_folder, LLInventoryCategory* dest_folder, LLInventoryItem* inv_item, std::string& tooltip_msg, S32 bundle_size = 1, bool from_paste = false);
bool can_move_folder_to_marketplace(const LLInventoryCategory* root_folder, LLInventoryCategory* dest_folder, LLInventoryCategory* inv_cat, std::string& tooltip_msg, S32 bundle_size = 1, bool check_items = true, bool from_paste = false);
bool move_item_to_marketplacelistings(LLInventoryItem* inv_item, LLUUID dest_folder, bool copy = false);
bool move_folder_to_marketplacelistings(LLInventoryCategory* inv_cat, const LLUUID& dest_folder, bool copy = false, bool move_no_copy_items = false);

S32  depth_nesting_in_marketplace(LLUUID cur_uuid);
LLUUID nested_parent_id(LLUUID cur_uuid, S32 depth);
S32 compute_stock_count(LLUUID cat_uuid, bool force_count = false);

void change_item_parent(const LLUUID& item_id, const LLUUID& new_parent_id);
void move_items_to_new_subfolder(const uuid_vec_t& selected_uuids, const std::string& folder_name);
void move_items_to_folder(const LLUUID& new_cat_uuid, const uuid_vec_t& selected_uuids);
bool is_only_cats_selected(const uuid_vec_t& selected_uuids);
bool is_only_items_selected(const uuid_vec_t& selected_uuids);
std::string get_category_path(LLUUID cat_id);

bool can_move_to_outfit(LLInventoryItem* inv_item, bool move_is_into_current_outfit);
bool can_move_to_landmarks(LLInventoryItem* inv_item);
bool can_move_to_my_outfits_as_outfit(LLInventoryModel* model, LLInventoryCategory* inv_cat, U32 wear_limit);
bool can_move_to_my_outfits_as_subfolder(LLInventoryModel* model, LLInventoryCategory* inv_cat, S32 depth = 0);
std::string get_localized_folder_name(LLUUID cat_uuid);
void new_folder_window(const LLUUID& folder_id);
void ungroup_folder_items(const LLUUID& folder_id);
std::string get_searchable_description(LLInventoryModel* model, const LLUUID& item_id);
std::string get_searchable_creator_name(LLInventoryModel* model, const LLUUID& item_id);
std::string get_searchable_UUID(LLInventoryModel* model, const LLUUID& item_id);
bool can_share_item(const LLUUID& item_id);

enum EMyOutfitsSubfolderType
{
    MY_OUTFITS_NO,
    MY_OUTFITS_SUBFOLDER,
    MY_OUTFITS_OUTFIT,
    MY_OUTFITS_SUBOUTFIT,
};
EMyOutfitsSubfolderType myoutfit_object_subfolder_type(
    LLInventoryModel* model,
    const LLUUID& obj_id,
    const LLUUID& my_outfits_id);

/**                    Miscellaneous global functions
 **                                                                            **
 *******************************************************************************/

class LLMarketplaceValidator: public LLSingleton<LLMarketplaceValidator>
{
    LLSINGLETON(LLMarketplaceValidator);
    ~LLMarketplaceValidator();
    LOG_CLASS(LLMarketplaceValidator);
public:

    typedef boost::function<void(std::string& validation_message, S32 depth, LLError::ELevel log_level)> validation_msg_callback_t;
    typedef boost::function<void(bool result)> validation_done_callback_t;

    void validateMarketplaceListings(
        const LLUUID &category_id,
        validation_done_callback_t cb_done = NULL,
        validation_msg_callback_t cb_msg = NULL,
        bool fix_hierarchy = true,
        S32 depth = -1);

private:
    void start();

    class ValidationRequest
    {
    public:
        ValidationRequest(
            LLUUID category_id,
            validation_done_callback_t cb_done,
            validation_msg_callback_t cb_msg,
            bool fix_hierarchy,
            S32 depth);
        LLUUID mCategoryId;
        validation_done_callback_t mCbDone;
        validation_msg_callback_t  mCbMsg;
        bool mFixHierarchy;
        S32 mDepth;
    };

    bool mValidationInProgress;
    S32 mPendingCallbacks;
    bool mPendingResult;
    // todo: might be a good idea to memorize requests by id and
    // filter out ones that got multiple validation requests
    std::queue<ValidationRequest> mValidationQueue;
};

/********************************************************************************
 **                                                                            **
 **                    INVENTORY COLLECTOR FUNCTIONS
 **/

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLInventoryCollectFunctor
//
// Base class for LLInventoryModel::collectDescendentsIf() method
// which accepts an instance of one of these objects to use as the
// function to determine if it should be added. Derive from this class
// and override the () operator to return true if you want to collect
// the category or item passed in.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLInventoryCollectFunctor
{
public:
    virtual ~LLInventoryCollectFunctor(){};
    virtual bool operator()(LLInventoryCategory* cat, LLInventoryItem* item) = 0;

    static bool itemTransferCommonlyAllowed(const LLInventoryItem* item);
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLAssetIDMatches
//
// This functor finds inventory items pointing to the specified asset
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLViewerInventoryItem;

class LLAssetIDMatches : public LLInventoryCollectFunctor
{
public:
    LLAssetIDMatches(const LLUUID& asset_id) : mAssetID(asset_id) {}
    virtual ~LLAssetIDMatches() {}
    bool operator()(LLInventoryCategory* cat, LLInventoryItem* item);

protected:
    LLUUID mAssetID;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLLinkedItemIDMatches
//
// This functor finds inventory items linked to the specific inventory id.
// Assumes the inventory id is itself not a linked item.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLLinkedItemIDMatches : public LLInventoryCollectFunctor
{
public:
    LLLinkedItemIDMatches(const LLUUID& item_id) : mBaseItemID(item_id) {}
    virtual ~LLLinkedItemIDMatches() {}
    bool operator()(LLInventoryCategory* cat, LLInventoryItem* item);

protected:
    LLUUID mBaseItemID;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLIsType
//
// Implementation of a LLInventoryCollectFunctor which returns true if
// the type is the type passed in during construction.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

class LLIsFolderType : public LLInventoryCollectFunctor
{
public:
    LLIsFolderType(LLFolderType::EType type) : mType(type) {}
    virtual ~LLIsFolderType() {}
    virtual bool operator()(LLInventoryCategory* cat,
        LLInventoryItem* item);
protected:
    LLFolderType::EType mType;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLIsType
//
// Implementation of a LLInventoryCollectFunctor which returns true if
// the type is the type passed in during construction.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

class LLIsType : public LLInventoryCollectFunctor
{
public:
    LLIsType(LLAssetType::EType type) : mType(type) {}
    virtual ~LLIsType() {}
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item);
protected:
    LLAssetType::EType mType;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLIsOneOfTypes
//
// Implementation of a LLInventoryCollectFunctor which returns true if
// the type is one of the types passed in during construction.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

class LLIsOneOfTypes : public LLInventoryCollectFunctor
{
public:
    LLIsOneOfTypes(const std::vector<LLAssetType::EType> &types) : mTypes(types) {}
    virtual ~LLIsOneOfTypes() {}
    virtual bool operator()(LLInventoryCategory* cat,
        LLInventoryItem* item);
protected:
    std::vector <LLAssetType::EType> mTypes;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLIsNotType
//
// Implementation of a LLInventoryCollectFunctor which returns false if the
// type is the type passed in during construction, otherwise false.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLIsNotType : public LLInventoryCollectFunctor
{
public:
    LLIsNotType(LLAssetType::EType type) : mType(type) {}
    virtual ~LLIsNotType() {}
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item);
protected:
    LLAssetType::EType mType;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLIsOfAssetType
//
// Implementation of a LLInventoryCollectFunctor which returns true if
// the item or category is of asset type passed in during construction.
// Link types are treated as links, not as the types they point to.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

class LLIsOfAssetType : public LLInventoryCollectFunctor
{
public:
    LLIsOfAssetType(LLAssetType::EType type) : mType(type) {}
    virtual ~LLIsOfAssetType() {}
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item);
protected:
    LLAssetType::EType mType;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLAssetIDAndTypeMatches
//
// Implementation of a LLInventoryCollectFunctor which returns true if
// the item matches both asset type and asset id.
// This is needed in case you are looking for a specific type with default id
// (since null is default for multiple asset types)
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

class LLAssetIDAndTypeMatches: public LLInventoryCollectFunctor
{
public:
    LLAssetIDAndTypeMatches(const LLUUID& asset_id, LLAssetType::EType type): mAssetID(asset_id), mType(type) {}
    virtual ~LLAssetIDAndTypeMatches() {}
    bool operator()(LLInventoryCategory* cat,
                    LLInventoryItem* item);

protected:
    LLUUID mAssetID;
    LLAssetType::EType mType;
};

class LLIsValidItemLink : public LLInventoryCollectFunctor
{
public:
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item);
};

class LLIsTypeWithPermissions : public LLInventoryCollectFunctor
{
public:
    LLIsTypeWithPermissions(LLAssetType::EType type, const PermissionBit perms, const LLUUID &agent_id, const LLUUID &group_id)
        : mType(type), mPerm(perms), mAgentID(agent_id), mGroupID(group_id) {}
    virtual ~LLIsTypeWithPermissions() {}
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item);
protected:
    LLAssetType::EType mType;
    PermissionBit mPerm;
    LLUUID          mAgentID;
    LLUUID          mGroupID;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLBuddyCollector
//
// Simple class that collects calling cards that are not null, and not
// the agent. Duplicates are possible.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLBuddyCollector : public LLInventoryCollectFunctor
{
public:
    LLBuddyCollector() {}
    virtual ~LLBuddyCollector() {}
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item);
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLUniqueBuddyCollector
//
// Simple class that collects calling cards that are not null, and not
// the agent. Duplicates are discarded.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLUniqueBuddyCollector : public LLInventoryCollectFunctor
{
public:
    LLUniqueBuddyCollector() {}
    virtual ~LLUniqueBuddyCollector() {}
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item);

protected:
    std::set<LLUUID> mSeen;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLParticularBuddyCollector
//
// Simple class that collects calling cards that match a particular uuid
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

class LLParticularBuddyCollector : public LLInventoryCollectFunctor
{
public:
    LLParticularBuddyCollector(const LLUUID& id) : mBuddyID(id) {}
    virtual ~LLParticularBuddyCollector() {}
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item);
protected:
    LLUUID mBuddyID;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLNameCategoryCollector
//
// Collects categories based on case-insensitive match of prefix
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLNameCategoryCollector : public LLInventoryCollectFunctor
{
public:
    LLNameCategoryCollector(const std::string& name) : mName(name) {}
    virtual ~LLNameCategoryCollector() {}
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item);
protected:
    std::string mName;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLFindCOFValidItems
//
// Collects items that can be legitimately linked to in the COF.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLFindCOFValidItems : public LLInventoryCollectFunctor
{
public:
    LLFindCOFValidItems() {}
    virtual ~LLFindCOFValidItems() {}
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item);

};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLFindBrokenLinks
//
// Collects broken links
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLFindBrokenLinks : public LLInventoryCollectFunctor
{
public:
    LLFindBrokenLinks() {}
    virtual ~LLFindBrokenLinks() {}
    virtual bool operator()(LLInventoryCategory* cat,
        LLInventoryItem* item);
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLFindByMask
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLFindByMask : public LLInventoryCollectFunctor
{
public:
    LLFindByMask(U64 mask)
        : mFilterMask(mask)
    {}

    virtual bool operator()(LLInventoryCategory* cat, LLInventoryItem* item)
    {
        //converting an inventory type to a bitmap filter mask
        if(item && (mFilterMask & (1LL << item->getInventoryType())) )
        {
            return true;
        }

        return false;
    }

private:
    U64 mFilterMask;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLFindNonLinksByMask
//
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLFindNonLinksByMask : public LLInventoryCollectFunctor
{
public:
    LLFindNonLinksByMask(U64 mask)
        : mFilterMask(mask)
    {}

    virtual bool operator()(LLInventoryCategory* cat, LLInventoryItem* item)
    {
        if(item && !item->getIsLinkType() && (mFilterMask & (1LL << item->getInventoryType())) )
        {
            return true;
        }

        return false;
    }

    void setFilterMask(U64 mask)
    {
        mFilterMask = mask;
    }

private:
    U64 mFilterMask;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLFindWearables
//
// Collects wearables based on item type.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLFindWearables : public LLInventoryCollectFunctor
{
public:
    LLFindWearables() {}
    virtual ~LLFindWearables() {}
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item);
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLFindWearablesEx
//
// Collects wearables based on given criteria.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLFindWearablesEx : public LLInventoryCollectFunctor
{
public:
    LLFindWearablesEx(bool is_worn, bool include_body_parts = true);
    virtual bool operator()(LLInventoryCategory* cat, LLInventoryItem* item);
private:
    bool mIncludeBodyParts;
    bool mIsWorn;
};

//Inventory collect functor collecting wearables of a specific wearable type
class LLFindWearablesOfType : public LLInventoryCollectFunctor
{
public:
    LLFindWearablesOfType(LLWearableType::EType type) : mWearableType(type) {}
    virtual ~LLFindWearablesOfType() {}
    virtual bool operator()(LLInventoryCategory* cat, LLInventoryItem* item);
    void setType(LLWearableType::EType type);

private:
    LLWearableType::EType mWearableType;
};

class LLIsTextureType : public LLInventoryCollectFunctor
{
public:
    LLIsTextureType() {}
    virtual ~LLIsTextureType() {}
    virtual bool operator()(LLInventoryCategory* cat,
        LLInventoryItem* item);
};

/** Filter out wearables-links */
class LLFindActualWearablesOfType : public LLFindWearablesOfType
{
public:
    LLFindActualWearablesOfType(LLWearableType::EType type) : LLFindWearablesOfType(type) {}
    virtual ~LLFindActualWearablesOfType() {}
    virtual bool operator()(LLInventoryCategory* cat, LLInventoryItem* item)
    {
        if (item && item->getIsLinkType()) return false;
        return LLFindWearablesOfType::operator()(cat, item);
    }
};

/* Filters out items of a particular asset type */
class LLIsTypeActual : public LLIsType
{
public:
    LLIsTypeActual(LLAssetType::EType type) : LLIsType(type) {}
    virtual ~LLIsTypeActual() {}
    virtual bool operator()(LLInventoryCategory* cat, LLInventoryItem* item)
    {
        if (item && item->getIsLinkType()) return false;
        return LLIsType::operator()(cat, item);
    }
};

// Collect non-removable folders and items.
class LLFindNonRemovableObjects : public LLInventoryCollectFunctor
{
public:
    virtual bool operator()(LLInventoryCategory* cat, LLInventoryItem* item);
};

// [SL:KB] - Patch: UI-Misc | Checked: 2014-03-02 (Catznip-3.6)
class LLFindLandmarks : public  LLInventoryCollectFunctor
{
public:
    LLFindLandmarks(bool fFilterDuplicates, bool fFilterSelf);
    virtual ~LLFindLandmarks() { }

    /*virtual*/ bool operator()(LLInventoryCategory* cat, LLInventoryItem* item);

protected:
    bool              m_fFilterDuplicates;
    std::list<LLUUID> m_AssetIds;
    bool              m_fFilterSelf;
};
// [/SL:KB]

/**                    Inventory Collector Functions
 **                                                                            **
 *******************************************************************************/
class LLFolderViewItem;
class LLFolderViewFolder;
class LLInventoryModel;
class LLFolderView;

class LLInventoryState
{
public:
    // HACK: Until we can route this info through the instant message hierarchy
    static bool sWearNewClothing;
    static LLUUID sWearNewClothingTransactionID;    // wear all clothing in this transaction
};

struct LLInventoryAction
{
    static void doToSelected(LLInventoryModel* model, LLFolderView* root, const std::string& action, bool user_confirm = true);
    static void callback_doToSelected(const LLSD& notification, const LLSD& response, class LLInventoryModel* model, class LLFolderView* root, const std::string& action);
    static void callback_copySelected(const LLSD& notification, const LLSD& response, class LLInventoryModel* model, class LLFolderView* root, const std::string& action);
    static void onItemsRemovalConfirmation(const LLSD& notification, const LLSD& response, LLHandle<LLFolderView> root);
    static void removeItemFromDND(LLFolderView* root);

    static void saveMultipleTextures(const std::vector<std::string>& filenames, std::set<LLFolderViewItem*> selected_items, LLInventoryModel* model);

    // <FS:Ansariel> Undo delete item confirmation per-session annoyance
    //static bool sDeleteConfirmationDisplayed;

private:
    static void buildMarketplaceFolders(LLFolderView* root);
    static void updateMarketplaceFolders();
    static std::list<LLUUID> sMarketplaceFolders; // Marketplace folders that will need update once the action is completed
};


#endif // LL_LLINVENTORYFUNCTIONS_H



