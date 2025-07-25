/**
 * @file llwearableitemslist.cpp
 * @brief A flat list of wearable items.
 *
 * $LicenseInfo:firstyear=2010&license=viewerlgpl$
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

#include "llwearableitemslist.h"

#include "lliconctrl.h"
#include "llmenugl.h" // for LLContextMenu

#include "llagentwearables.h"
#include "llappearancemgr.h"
#include "llinventoryicon.h"
#include "llgesturemgr.h"
#include "lltransutil.h"
#include "llviewerattachmenu.h"
#include "llviewermenu.h"
#include "llvoavatarself.h"
// [RLVa:KB] - Checked: 2011-05-22 (RLVa-1.3.1a)
#include "rlvactions.h"
#include "rlvlocks.h"
// [/RLVa:KB]
#include "lltextbox.h"
#include "llresmgr.h"

bool LLFindOutfitItems::operator()(LLInventoryCategory* cat,
                                   LLInventoryItem* item)
{
    if(item)
    {
        if((item->getType() == LLAssetType::AT_CLOTHING)
           || (item->getType() == LLAssetType::AT_BODYPART)
           || (item->getType() == LLAssetType::AT_OBJECT)
           || (item->getType() == LLAssetType::AT_GESTURE))
        {
            return true;
        }
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void LLPanelWearableListItem::onMouseEnter(S32 x, S32 y, MASK mask)
{
    LLPanelInventoryListItemBase::onMouseEnter(x, y, mask);
    setWidgetsVisible(true);
    reshapeWidgets();
}

void LLPanelWearableListItem::onMouseLeave(S32 x, S32 y, MASK mask)
{
    LLPanelInventoryListItemBase::onMouseLeave(x, y, mask);
    setWidgetsVisible(false);
    reshapeWidgets();
}

LLPanelWearableListItem::LLPanelWearableListItem(LLViewerInventoryItem* item, const LLPanelWearableListItem::Params& params)
: LLPanelInventoryListItemBase(item, params)
{
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
static LLWidgetNameRegistry::StaticRegistrar sRegisterPanelWearableOutfitItem(&typeid(LLPanelWearableOutfitItem::Params), "wearable_outfit_list_item");

LLPanelWearableOutfitItem::Params::Params()
:   add_btn("add_btn"),
    remove_btn("remove_btn")
{
}

bool LLPanelWearableOutfitItem::postBuild()
{
    if (mShowWidgets)
    {
        mAddWearableBtn = getChild<LLButton>("add_wearable");
        mRemoveWearableBtn = getChild<LLButton>("remove_wearable");
    }

    LLPanelWearableListItem::postBuild();

    if(mShowWidgets)
    {
        addWidgetToRightSide(mAddWearableBtn);
        addWidgetToRightSide(mRemoveWearableBtn);

        mAddWearableBtn->setClickedCallback(boost::bind(&LLPanelWearableOutfitItem::onAddWearable, this));
        mRemoveWearableBtn->setClickedCallback(boost::bind(&LLPanelWearableOutfitItem::onRemoveWearable, this));

        setWidgetsVisible(false);
        reshapeWidgets();
    }
    return true;
}

bool LLPanelWearableOutfitItem::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if(!mShowWidgets)
    {
        return LLPanelWearableListItem::handleDoubleClick(x, y, mask);
    }

    if(LLAppearanceMgr::instance().isLinkedInCOF(mInventoryItemUUID))
    {
        onRemoveWearable();
    }
    else
    {
        onAddWearable();
    }
    return true;
}

void LLPanelWearableOutfitItem::onAddWearable()
{
    setWidgetsVisible(false);
    reshapeWidgets();
    LLAppearanceMgr::instance().wearItemOnAvatar(mInventoryItemUUID, true, false);
}

void LLPanelWearableOutfitItem::onRemoveWearable()
{
    setWidgetsVisible(false);
    reshapeWidgets();
    LLAppearanceMgr::instance().removeItemFromAvatar(mInventoryItemUUID);
}

// static
LLPanelWearableOutfitItem* LLPanelWearableOutfitItem::create(LLViewerInventoryItem* item,
                                                             bool worn_indication_enabled,
                                                             bool show_widgets)
{
    LLPanelWearableOutfitItem* list_item = NULL;
    if (item)
    {
        const LLPanelWearableOutfitItem::Params& params = LLUICtrlFactory::getDefaultParams<LLPanelWearableOutfitItem>();

        list_item = new LLPanelWearableOutfitItem(item, worn_indication_enabled, params, show_widgets);
        list_item->initFromParams(params);
        list_item->postBuild();
    }
    return list_item;
}

LLPanelWearableOutfitItem::LLPanelWearableOutfitItem(LLViewerInventoryItem* item,
                                                     bool worn_indication_enabled,
                                                     const LLPanelWearableOutfitItem::Params& params,
                                                     bool show_widgets)
: LLPanelWearableListItem(item, params)
, mWornIndicationEnabled(worn_indication_enabled)
, mShowWidgets(show_widgets)
{
    if(mShowWidgets)
    {
        LLButton::Params button_params = params.add_btn;
        applyXUILayout(button_params, this);
        addChild(LLUICtrlFactory::create<LLButton>(button_params));

        button_params = params.remove_btn;
        applyXUILayout(button_params, this);
        addChild(LLUICtrlFactory::create<LLButton>(button_params));
    }
}

// virtual
void LLPanelWearableOutfitItem::updateItem(const std::string& name,
                                           EItemState item_state)
{
    std::string search_label = name;

    // Updating item's worn status depending on whether it is linked in COF or not.
    // We don't use get_is_item_worn() here because this update is triggered by
    // an inventory observer upon link in COF beind added or removed so actual
    // worn status of a linked item may still remain unchanged.
    bool is_worn = LLAppearanceMgr::instance().isLinkedInCOF(mInventoryItemUUID);
    // <FS:Ansariel> Better attachment list
    //if (mWornIndicationEnabled && is_worn)
    //{
    //  search_label += LLTrans::getString("worn");
    //  item_state = IS_WORN;
    //}
    if (mWornIndicationEnabled)
    {
        if (getType() == LLAssetType::AT_OBJECT && get_is_item_worn(mInventoryItemUUID))
        {
            std::string attachment_point_name;
            if (!isAgentAvatarValid())
            {
                search_label += LLTrans::getString("worn");
            }
            else if (gAgentAvatarp->getAttachedPointName(mInventoryItemUUID, attachment_point_name))
            {
                LLStringUtil::format_map_t args;
                args["[ATTACHMENT_POINT]"] = LLTrans::getString(attachment_point_name);
                search_label += LLTrans::getString("WornOnAttachmentPoint", args);
            }
            else
            {
                LLStringUtil::format_map_t args;
                args["[ATTACHMENT_ERROR]"] = LLTrans::getString(attachment_point_name);
                search_label += LLTrans::getString("AttachmentErrorMessage", args);
            }

            item_state = is_worn ? IS_WORN : IS_MISMATCH;
        }
        else if (getType() != LLAssetType::AT_OBJECT && is_worn)
        {
            search_label += LLTrans::getString("worn");
            item_state = IS_WORN;
        }
    }
    // </FS:Ansariel>

    if(mShowWidgets)
    {
        setShowWidget(mAddWearableBtn, !is_worn);

        // Body parts can't be removed, only replaced
        LLViewerInventoryItem* inv_item = getItem();
        bool show_remove = is_worn && inv_item && (inv_item->getType() != LLAssetType::AT_BODYPART);
        setShowWidget(mRemoveWearableBtn, show_remove);

        if(mHovered)
        {
            setWidgetsVisible(true);
            reshapeWidgets();
        }
    }

    LLPanelInventoryListItemBase::updateItem(search_label, item_state);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
static LLWidgetNameRegistry::StaticRegistrar sRegisterPanelClothingListItem(&typeid(LLPanelClothingListItem::Params), "clothing_list_item");


LLPanelClothingListItem::Params::Params()
:   up_btn("up_btn"),
    down_btn("down_btn"),
    edit_btn("edit_btn"),
    lock_panel("lock_panel"),
    edit_panel("edit_panel"),
    lock_icon("lock_icon")
{}

// static
LLPanelClothingListItem* LLPanelClothingListItem::create(LLViewerInventoryItem* item)
{
    LLPanelClothingListItem* list_item = NULL;
    if(item)
    {
        const LLPanelClothingListItem::Params& params = LLUICtrlFactory::getDefaultParams<LLPanelClothingListItem>();
        list_item = new LLPanelClothingListItem(item, params);
        list_item->initFromParams(params);
        list_item->postBuild();
    }
    return list_item;
}

LLPanelClothingListItem::LLPanelClothingListItem(LLViewerInventoryItem* item, const LLPanelClothingListItem::Params& params)
 : LLPanelDeletableWearableListItem(item, params)
{
    LLButton::Params button_params = params.up_btn;
    applyXUILayout(button_params, this);
    addChild(LLUICtrlFactory::create<LLButton>(button_params));

    button_params = params.down_btn;
    applyXUILayout(button_params, this);
    addChild(LLUICtrlFactory::create<LLButton>(button_params));

    LLPanel::Params panel_params = params.lock_panel;
    applyXUILayout(panel_params, this);
    LLPanel* lock_panelp = LLUICtrlFactory::create<LLPanel>(panel_params);
    addChild(lock_panelp);

    panel_params = params.edit_panel;
    applyXUILayout(panel_params, this);
    LLPanel* edit_panelp = LLUICtrlFactory::create<LLPanel>(panel_params);
    addChild(edit_panelp);

    if (lock_panelp)
{
        LLIconCtrl::Params icon_params = params.lock_icon;
        applyXUILayout(icon_params, this);
        lock_panelp->addChild(LLUICtrlFactory::create<LLIconCtrl>(icon_params));
}

    if (edit_panelp)
{
        button_params = params.edit_btn;
        applyXUILayout(button_params, this);
        edit_panelp->addChild(LLUICtrlFactory::create<LLButton>(button_params));
    }

    setSeparatorVisible(false);
}

LLPanelClothingListItem::~LLPanelClothingListItem()
{
}

bool LLPanelClothingListItem::postBuild()
{
    LLPanelDeletableWearableListItem::postBuild();

    addWidgetToRightSide("btn_move_up");
    addWidgetToRightSide("btn_move_down");
    addWidgetToRightSide("btn_lock");
    addWidgetToRightSide("btn_edit_panel");

    setWidgetsVisible(false);
    reshapeWidgets();

    return true;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static LLWidgetNameRegistry::StaticRegistrar sRegisterPanelBodyPartsListItem(&typeid(LLPanelBodyPartsListItem::Params), "bodyparts_list_item");


LLPanelBodyPartsListItem::Params::Params()
:   edit_btn("edit_btn"),
    edit_panel("edit_panel"),
    lock_panel("lock_panel"),
    lock_icon("lock_icon")
{}

// static
LLPanelBodyPartsListItem* LLPanelBodyPartsListItem::create(LLViewerInventoryItem* item)
{
    LLPanelBodyPartsListItem* list_item = NULL;
    if(item)
    {
        const Params& params = LLUICtrlFactory::getDefaultParams<LLPanelBodyPartsListItem>();
        list_item = new LLPanelBodyPartsListItem(item, params);
        list_item->initFromParams(params);
        list_item->postBuild();
    }
    return list_item;
}

LLPanelBodyPartsListItem::LLPanelBodyPartsListItem(LLViewerInventoryItem* item, const LLPanelBodyPartsListItem::Params& params)
: LLPanelWearableListItem(item, params)
{
    LLPanel::Params panel_params = params.edit_panel;
    applyXUILayout(panel_params, this);
    LLPanel* edit_panelp = LLUICtrlFactory::create<LLPanel>(panel_params);
    addChild(edit_panelp);

    panel_params = params.lock_panel;
    applyXUILayout(panel_params, this);
    LLPanel* lock_panelp = LLUICtrlFactory::create<LLPanel>(panel_params);
    addChild(lock_panelp);

    if (edit_panelp)
    {
        LLButton::Params btn_params = params.edit_btn;
        applyXUILayout(btn_params, this);
        edit_panelp->addChild(LLUICtrlFactory::create<LLButton>(btn_params));
}

    if (lock_panelp)
{
        LLIconCtrl::Params icon_params = params.lock_icon;
        applyXUILayout(icon_params, this);
        lock_panelp->addChild(LLUICtrlFactory::create<LLIconCtrl>(icon_params));
    }

    setSeparatorVisible(true);
}

LLPanelBodyPartsListItem::~LLPanelBodyPartsListItem()
{
}

bool LLPanelBodyPartsListItem::postBuild()
{
    LLPanelInventoryListItemBase::postBuild();

    addWidgetToRightSide("btn_lock");
    addWidgetToRightSide("btn_edit_panel");

    setWidgetsVisible(false);
    reshapeWidgets();

    return true;
}

static LLWidgetNameRegistry::StaticRegistrar sRegisterPanelDeletableWearableListItem(&typeid(LLPanelDeletableWearableListItem::Params), "deletable_wearable_list_item");

LLPanelDeletableWearableListItem::Params::Params()
:   delete_btn("delete_btn")
{}

// static
LLPanelDeletableWearableListItem* LLPanelDeletableWearableListItem::create(LLViewerInventoryItem* item)
{
    LLPanelDeletableWearableListItem* list_item = NULL;
    if(item)
    {
        const Params& params = LLUICtrlFactory::getDefaultParams<LLPanelDeletableWearableListItem>();
        list_item = new LLPanelDeletableWearableListItem(item, params);
        list_item->initFromParams(params);
        list_item->postBuild();
    }
    return list_item;
}

LLPanelDeletableWearableListItem::LLPanelDeletableWearableListItem(LLViewerInventoryItem* item, const LLPanelDeletableWearableListItem::Params& params)
: LLPanelWearableListItem(item, params)
{
    LLButton::Params button_params = params.delete_btn;
    applyXUILayout(button_params, this);
    addChild(LLUICtrlFactory::create<LLButton>(button_params));

    setSeparatorVisible(true);
}

bool LLPanelDeletableWearableListItem::postBuild()
{
    LLPanelWearableListItem::postBuild();

    addWidgetToLeftSide("btn_delete");

    LLButton* delete_btn = getChild<LLButton>("btn_delete");
    // Reserve space for 'delete' button event if it is invisible.
    setLeftWidgetsWidth(delete_btn->getRect().mRight);

    setWidgetsVisible(false);
    reshapeWidgets();

    return true;
}


// static
LLPanelAttachmentListItem* LLPanelAttachmentListItem::create(LLViewerInventoryItem* item)
{
    LLPanelAttachmentListItem* list_item = NULL;
    if(item)
    {
        const Params& params = LLUICtrlFactory::getDefaultParams<LLPanelDeletableWearableListItem>();

        list_item = new LLPanelAttachmentListItem(item, params);
        list_item->initFromParams(params);
        list_item->postBuild();
    }
    return list_item;
}

void LLPanelAttachmentListItem::updateItem(const std::string& name,
                                           EItemState item_state)
{
    std::string title_joint = name;

    LLViewerInventoryItem* inv_item = getItem();
    if (inv_item && isAgentAvatarValid() && gAgentAvatarp->isWearingAttachment(inv_item->getLinkedUUID()))
    {
        std::string found_name;
        bool found = gAgentAvatarp->getAttachedPointName(inv_item->getLinkedUUID(),found_name);
        std::string trans_name = LLTrans::getString(found_name);
        if (!found)
        {
            LL_WARNS() << "invalid attachment joint, err " << found_name << LL_ENDL;
        }
        title_joint =  title_joint + " (" + trans_name + ")";
    }

    LLPanelInventoryListItemBase::updateItem(title_joint, item_state);
}

// <FS:Ansariel> Show per-item complexity in COF
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
static LLWidgetNameRegistry::StaticRegistrar sRegisterPanelCOFWearableOutfitListItem(&typeid(FSPanelCOFWearableOutfitListItem::Params), "cof_wearable_list_item");

FSPanelCOFWearableOutfitListItem::Params::Params()
:   item_weight("item_weight")
{}

// static
FSPanelCOFWearableOutfitListItem* FSPanelCOFWearableOutfitListItem::create(LLViewerInventoryItem* item,
                                                             bool worn_indication_enabled, bool show_widgets, U32 weight)
{
    FSPanelCOFWearableOutfitListItem* list_item = NULL;
    if(item)
    {
        const Params& params = LLUICtrlFactory::getDefaultParams<FSPanelCOFWearableOutfitListItem>();
        list_item = new FSPanelCOFWearableOutfitListItem(item, worn_indication_enabled, show_widgets, params);
        list_item->initFromParams(params);
        list_item->postBuild();
        list_item->updateItemWeight(weight);
    }
    return list_item;
}


FSPanelCOFWearableOutfitListItem::FSPanelCOFWearableOutfitListItem(LLViewerInventoryItem* item,
                                                     bool worn_indication_enabled,
                                                     bool show_widgets,
                                                     const FSPanelCOFWearableOutfitListItem::Params& params)
: LLPanelWearableOutfitItem(item, worn_indication_enabled, params, show_widgets)
, mWeightCtrl(nullptr)
{
    LLTextBox::Params weight_params = params.item_weight;
    applyXUILayout(weight_params, this);
    addChild(LLUICtrlFactory::create<LLTextBox>(weight_params));
}

bool FSPanelCOFWearableOutfitListItem::postBuild()
{
    mWeightCtrl = getChild<LLTextBox>("item_weight");

    if (!LLPanelWearableOutfitItem::postBuild())
    {
        return false;
    }

    addWidgetToRightSide(mWeightCtrl);

    // Reserve space for 'delete' button event if it is invisible.
    setRightWidgetsWidth(mWeightCtrl->getRect().getWidth() + 5);

    mWeightCtrl->setVisible(true);

    reshapeWidgets();

    return true;
}

void FSPanelCOFWearableOutfitListItem::updateItemWeight(U32 item_weight)
{
    std::string complexity_string;
    if (item_weight > 0)
    {
        LLLocale locale("");
        LLResMgr::getInstance()->getIntegerString(complexity_string, item_weight);
    }
    mWeightCtrl->setText(complexity_string);
}

//virtual
void FSPanelCOFWearableOutfitListItem::updateItem(const std::string& name, EItemState item_state)
{
    LLPanelWearableOutfitItem::updateItem(name, item_state);
    mWeightCtrl->setVisible(true);
    reshapeWidgets();
}

//virtual
void FSPanelCOFWearableOutfitListItem::onMouseLeave(S32 x, S32 y, MASK mask)
{
    LLPanelInventoryListItemBase::onMouseLeave(x, y, mask);
    setWidgetsVisible(false);
    mWeightCtrl->setVisible(true); // setWidgetsVisible sets this invisible - make it visible again
    reshapeWidgets();
}

//virtual
const LLPanelInventoryListItemBase::Params& FSPanelCOFWearableOutfitListItem::getDefaultParams() const
{
    return LLUICtrlFactory::getDefaultParams<FSPanelCOFWearableOutfitListItem>();
}
// </FS:Ansariel>

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
static LLWidgetNameRegistry::StaticRegistrar sRegisterPanelDummyClothingListItem(&typeid(LLPanelDummyClothingListItem::Params), "dummy_clothing_list_item");

LLPanelDummyClothingListItem::Params::Params()
:   add_panel("add_panel"),
    add_btn("add_btn")
{}

LLPanelDummyClothingListItem* LLPanelDummyClothingListItem::create(LLWearableType::EType w_type)
{
    const Params& params = LLUICtrlFactory::getDefaultParams<LLPanelDummyClothingListItem>();

    LLPanelDummyClothingListItem* list_item = new LLPanelDummyClothingListItem(w_type, params);
    list_item->initFromParams(params);
    list_item->postBuild();
    return list_item;
}

bool LLPanelDummyClothingListItem::postBuild()
{
    addWidgetToRightSide("btn_add_panel");

    setIconImage(LLInventoryIcon::getIcon(LLAssetType::AT_CLOTHING, LLInventoryType::IT_NONE, mWearableType, false));
    updateItem(wearableTypeToString(mWearableType));

    // Make it look loke clothing item - reserve space for 'delete' button
    setLeftWidgetsWidth(getChildView("item_icon")->getRect().mLeft);

    setWidgetsVisible(false);
    reshapeWidgets();

    return true;
}

LLWearableType::EType LLPanelDummyClothingListItem::getWearableType() const
{
    return mWearableType;
}

LLPanelDummyClothingListItem::LLPanelDummyClothingListItem(LLWearableType::EType w_type, const LLPanelDummyClothingListItem::Params& params)
:   LLPanelWearableListItem(NULL, params),
    mWearableType(w_type)
{
    LLPanel::Params panel_params(params.add_panel);
    applyXUILayout(panel_params, this);
    LLPanel* add_panelp = LLUICtrlFactory::create<LLPanel>(panel_params);
    addChild(add_panelp);

    if (add_panelp)
{
        LLButton::Params button_params(params.add_btn);
        applyXUILayout(button_params, this);
        add_panelp->addChild(LLUICtrlFactory::create<LLButton>(button_params));
}

    setSeparatorVisible(true);
}

typedef std::map<LLWearableType::EType, std::string> clothing_to_string_map_t;

clothing_to_string_map_t init_clothing_string_map()
{
    clothing_to_string_map_t w_map;
    w_map.insert(std::make_pair(LLWearableType::WT_SHIRT, "shirt_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_PANTS, "pants_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_SHOES, "shoes_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_SOCKS, "socks_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_JACKET, "jacket_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_GLOVES, "gloves_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_UNDERSHIRT, "undershirt_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_UNDERPANTS, "underpants_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_SKIRT, "skirt_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_ALPHA, "alpha_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_TATTOO, "tattoo_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_UNIVERSAL, "universal_not_worn"));
    w_map.insert(std::make_pair(LLWearableType::WT_PHYSICS, "physics_not_worn"));
    return w_map;
}

std::string LLPanelDummyClothingListItem::wearableTypeToString(LLWearableType::EType w_type)
{
    static const clothing_to_string_map_t w_map = init_clothing_string_map();
    static const std::string invalid_str = LLTrans::getString("invalid_not_worn");

    std::string type_str = invalid_str;
    clothing_to_string_map_t::const_iterator it = w_map.find(w_type);
    if(w_map.end() != it)
    {
        type_str = LLTrans::getString(it->second);
    }
    return type_str;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

LLWearableItemTypeNameComparator::LLWearableTypeOrder::LLWearableTypeOrder(LLWearableItemTypeNameComparator::ETypeListOrder order_priority, bool sort_asset_by_name, bool sort_wearable_by_name):
        mOrderPriority(order_priority),
        mSortAssetTypeByName(sort_asset_by_name),
        mSortWearableTypeByName(sort_wearable_by_name)
{
}

LLWearableItemTypeNameComparator::LLWearableItemTypeNameComparator()
{
    // By default the sort order conforms the order by spec of MY OUTFITS items list:
    // 1. CLOTHING - sorted by name
    // 2. OBJECT   - sorted by type
    // 3. BODYPART - sorted by name
    mWearableOrder[LLAssetType::AT_CLOTHING] = LLWearableTypeOrder(ORDER_RANK_1, false, false);
    mWearableOrder[LLAssetType::AT_OBJECT]   = LLWearableTypeOrder(ORDER_RANK_2, true, true);
    mWearableOrder[LLAssetType::AT_BODYPART] = LLWearableTypeOrder(ORDER_RANK_3, false, true);
    mWearableOrder[LLAssetType::AT_GESTURE] = LLWearableTypeOrder(ORDER_RANK_4, true, false);
}

void LLWearableItemTypeNameComparator::setOrder(LLAssetType::EType items_of_type,  LLWearableItemTypeNameComparator::ETypeListOrder order_priority, bool sort_asset_items_by_name, bool sort_wearable_items_by_name)
{
    mWearableOrder[items_of_type] = LLWearableTypeOrder(order_priority, sort_asset_items_by_name, sort_wearable_items_by_name);
}

/*virtual*/
bool LLWearableItemNameComparator::doCompare(const LLPanelInventoryListItemBase* wearable_item1, const LLPanelInventoryListItemBase* wearable_item2) const
{
    std::string name1 = wearable_item1->getItemName();
    std::string name2 = wearable_item2->getItemName();

    LLStringUtil::toUpper(name1);
    LLStringUtil::toUpper(name2);

    return name1 < name2;
}

/*virtual*/
bool LLWearableItemTypeNameComparator::doCompare(const LLPanelInventoryListItemBase* wearable_item1, const LLPanelInventoryListItemBase* wearable_item2) const
{
    const LLAssetType::EType item_type1 = wearable_item1->getType();
    const LLAssetType::EType item_type2 = wearable_item2->getType();

    LLWearableItemTypeNameComparator::ETypeListOrder item_type_order1 = getTypeListOrder(item_type1);
    LLWearableItemTypeNameComparator::ETypeListOrder item_type_order2 = getTypeListOrder(item_type2);

    if (item_type_order1 != item_type_order2)
    {
        // If items are of different asset types we can compare them
        // by types order in the list.
        return item_type_order1 < item_type_order2;
    }

    if (sortAssetTypeByName(item_type1))
    {
        // If both items are of the same asset type except AT_CLOTHING and AT_BODYPART
        // we can compare them by name.
        return LLWearableItemNameComparator::doCompare(wearable_item1, wearable_item2);
    }

    const LLWearableType::EType item_wearable_type1 = wearable_item1->getWearableType();
    const LLWearableType::EType item_wearable_type2 = wearable_item2->getWearableType();

    if (item_wearable_type1 != item_wearable_type2)
        // If items are of different LLWearableType::EType types they are compared
        // by LLWearableType::EType. types order determined in LLWearableType::EType.
    {
        // If items are of different LLWearableType::EType types they are compared
        // by LLWearableType::EType. types order determined in LLWearableType::EType.
        return item_wearable_type1 < item_wearable_type2;
    }
    else
    {
        // If both items are of the same clothing type they are compared
        // by description and place in reverse order (i.e. outer layer item
        // on top) OR by name
        if(sortWearableTypeByName(item_type1))
        {
            return LLWearableItemNameComparator::doCompare(wearable_item1, wearable_item2);
        }
        return wearable_item1->getDescription() > wearable_item2->getDescription();
    }
}

LLWearableItemTypeNameComparator::ETypeListOrder LLWearableItemTypeNameComparator::getTypeListOrder(LLAssetType::EType item_type) const
{
    wearable_type_order_map_t::const_iterator const_it = mWearableOrder.find(item_type);


    if(const_it == mWearableOrder.end())
    {
        LL_WARNS()<<"Absent information about order rang of items of "<<LLAssetType::getDesc(item_type)<<" type"<<LL_ENDL;
        return ORDER_RANK_UNKNOWN;
    }

    return const_it->second.mOrderPriority;
}

bool LLWearableItemTypeNameComparator::sortAssetTypeByName(LLAssetType::EType item_type) const
{
    wearable_type_order_map_t::const_iterator const_it = mWearableOrder.find(item_type);


    if(const_it == mWearableOrder.end())
    {
        LL_WARNS()<<"Absent information about sorting items of "<<LLAssetType::getDesc(item_type)<<" type"<<LL_ENDL;
        return true;
    }


    return const_it->second.mSortAssetTypeByName;
    }


bool LLWearableItemTypeNameComparator::sortWearableTypeByName(LLAssetType::EType item_type) const
{
    wearable_type_order_map_t::const_iterator const_it = mWearableOrder.find(item_type);


    if(const_it == mWearableOrder.end())
    {
        LL_WARNS()<<"Absent information about sorting items of "<<LLAssetType::getDesc(item_type)<<" type"<<LL_ENDL;
        return true;
}


    return const_it->second.mSortWearableTypeByName;
}

/*virtual*/
bool LLWearableItemCreationDateComparator::doCompare(const LLPanelInventoryListItemBase* item1, const LLPanelInventoryListItemBase* item2) const
{
    time_t date1 = item1->getCreationDate();
    time_t date2 = item2->getCreationDate();

    if (date1 == date2)
    {
        return LLWearableItemNameComparator::doCompare(item1, item2);
    }

    return date1 > date2;
}
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static LLWearableItemTypeNameComparator WEARABLE_TYPE_NAME_COMPARATOR;
static const LLWearableItemTypeNameComparator WEARABLE_TYPE_LAYER_COMPARATOR;
static const LLWearableItemNameComparator WEARABLE_NAME_COMPARATOR;
//static const LLWearableItemCreationDateComparator WEARABLE_CREATION_DATE_COMPARATOR;
static LLWearableItemCreationDateComparator WEARABLE_CREATION_DATE_COMPARATOR;  // <ND/> const makes GCC >= 4.6 very angry about not user defined default ctor.

static const LLDefaultChildRegistry::Register<LLWearableItemsList> r("wearable_items_list");

LLWearableItemsList::Params::Params()
:   standalone("standalone", true)
,   worn_indication_enabled("worn_indication_enabled", true)
,   show_item_widgets("show_item_widgets", false)
,   show_create_new("show_create_new", true) // <FS:Ansariel> Optional "Create new" menu item
,   show_complexity("show_complexity", false) // <FS:Ansariel> Show per-item complexity in COF
{}

LLWearableItemsList::LLWearableItemsList(const LLWearableItemsList::Params& p)
:   LLInventoryItemsList(p)
, mAttachmentsChangedCallbackConnection() // <FS:Ansariel> Better attachment list
{
    setSortOrder(E_SORT_BY_TYPE_LAYER, false);
    mMenuWearableType = LLWearableType::WT_NONE;
    mIsStandalone = p.standalone;
    if (mIsStandalone)
    {
        // Use built-in context menu.
        setRightMouseDownCallback(boost::bind(&LLWearableItemsList::onRightClick, this, _2, _3));
    }
    mWornIndicationEnabled = p.worn_indication_enabled;
    setNoItemsCommentText(LLTrans::getString("LoadingData"));
    mShowItemWidgets = p.show_item_widgets;
    mShowCreateNew = p.show_create_new; // <FS:Ansariel> Optional "Create new" menu item
    // <FS:Ansariel> Show per-item complexity in COF
    mShowComplexity = p.show_complexity;
    mBodyPartsComplexity = 0;
    // </FS:Ansariel>

    // <FS:Ansariel> Better attachment list
    mAttachmentsChangedCallbackConnection = LLAppearanceMgr::instance().setAttachmentsChangedCallback(boost::bind(&LLWearableItemsList::updateChangedItem, this, _1));
}

// virtual
LLWearableItemsList::~LLWearableItemsList()
{
    // <FS:Ansariel> Better attachment list
    if (mAttachmentsChangedCallbackConnection.connected())
    {
        mAttachmentsChangedCallbackConnection.disconnect();
    }
    // </FS:Ansariel>
}

// virtual
LLPanel* LLWearableItemsList::createNewItem(LLViewerInventoryItem* item)
{
    if (!item)
    {
        LL_WARNS() << "No inventory item. Couldn't create flat list item." << LL_ENDL;
        llassert(item != NULL);
        return NULL;
    }

    // <FS:Ansariel> Show per-item complexity in COF
    //return LLPanelWearableOutfitItem::create(item, mWornIndicationEnabled, mShowItemWidgets);
    if (!mShowComplexity)
    {
        return LLPanelWearableOutfitItem::create(item, mWornIndicationEnabled, mShowItemWidgets);
    }
    else
    {
        U32 weight;
        if (item->getWearableType() == LLWearableType::WT_SKIN)
        {
            weight = mBodyPartsComplexity;
        }
        else
        {
            LLUUID linked_item_id = item->getLinkedUUID();
            mLinkedItemsMap[linked_item_id] = item->getUUID();
            weight = mItemComplexityMap[linked_item_id];
        }
        return FSPanelCOFWearableOutfitListItem::create(item, mWornIndicationEnabled, mShowItemWidgets, weight);
    }
    // </FS:Ansariel>
}

void LLWearableItemsList::updateList(const LLUUID& category_id)
{
    LLInventoryModel::cat_array_t cat_array;
    LLInventoryModel::item_array_t item_array;

    LLFindOutfitItems collector = LLFindOutfitItems();
    // collectDescendentsIf takes non-const reference:
    gInventory.collectDescendentsIf(
        category_id,
        cat_array,
        item_array,
        LLInventoryModel::EXCLUDE_TRASH,
        collector);

    if(item_array.empty() && gInventory.isCategoryComplete(category_id))
    {
        setNoItemsCommentText(LLTrans::getString("EmptyOutfitText"));
    }

    refreshList(item_array);
}

void LLWearableItemsList::updateChangedItems(const uuid_vec_t& changed_items_uuids)
{
    // nothing to update
    if (changed_items_uuids.empty())
        return;

    uuid_vec_t::const_iterator uuids_begin = changed_items_uuids.begin(), uuids_end = changed_items_uuids.end();
    pairs_const_iterator_t pairs_iter = getItemPairs().begin(), pairs_end = getItemPairs().end();
    while (pairs_iter != pairs_end)
    {
        LLPanel* panel = (*(pairs_iter++))->first;
        LLPanelInventoryListItemBase* item = dynamic_cast<LLPanelInventoryListItemBase*>(panel);
        if (!item)
            continue;

        LLViewerInventoryItem* inv_item = item->getItem();
        if (!inv_item)
            continue;

        const LLUUID& linked_uuid = inv_item->getLinkedUUID();
        if (std::find(uuids_begin, uuids_end, linked_uuid) != uuids_end)
        {
            item->setNeedsRefresh(true);
        }
    }
}

// <FS:Ansariel> Better attachment list
void LLWearableItemsList::updateChangedItem(const LLUUID& changed_item_uuid)
{
    uuid_vec_t items;
    items.push_back(changed_item_uuid);
    updateChangedItems(items);
}
// </FS:Ansariel>

void LLWearableItemsList::onRightClick(S32 x, S32 y)
{
    uuid_vec_t selected_uuids;

    getSelectedUUIDs(selected_uuids);
    if (selected_uuids.empty())
    {
        if ((mMenuWearableType != LLWearableType::WT_NONE) && (size() == 0))
        {
            ContextMenu::instance().show(this, mMenuWearableType, x, y);
        }
    }
    else
    {
        ContextMenu::instance().show(this, selected_uuids, x, y);
    }
}

void LLWearableItemsList::setSortOrder(ESortOrder sort_order, bool sort_now)
{
    switch (sort_order)
    {
    case E_SORT_BY_MOST_RECENT:
        setComparator(&WEARABLE_CREATION_DATE_COMPARATOR);
        break;
    case E_SORT_BY_NAME:
        setComparator(&WEARABLE_NAME_COMPARATOR);
        break;
    case E_SORT_BY_TYPE_LAYER:
        setComparator(&WEARABLE_TYPE_LAYER_COMPARATOR);
        break;
    case E_SORT_BY_TYPE_NAME:
    {
        WEARABLE_TYPE_NAME_COMPARATOR.setOrder(LLAssetType::AT_CLOTHING, LLWearableItemTypeNameComparator::ORDER_RANK_1, false, true);
        setComparator(&WEARABLE_TYPE_NAME_COMPARATOR);
        break;
    }

    // No "default:" to raise compiler warning
    // if we're not handling something
    }

    mSortOrder = sort_order;

    if (sort_now)
    {
        sort();
    }
}

// <FS:Ansariel> Show per-item complexity in COF
void LLWearableItemsList::updateItemComplexity(const std::map<LLUUID, U32>& item_complexity, U32 body_parts_complexity)
{
    if (mShowComplexity)
    {
        mItemComplexityMap = item_complexity;
        mBodyPartsComplexity = body_parts_complexity;
        updateComplexity();
    }
}

void LLWearableItemsList::updateComplexity()
{
    for (std::map<LLUUID, U32>::const_iterator it = mItemComplexityMap.begin(); it != mItemComplexityMap.end(); ++it)
    {
        LLUUID id = mLinkedItemsMap[it->first];
        LLPanel* panel = getItemByValue(id);
        if (panel)
        {
            FSPanelCOFWearableOutfitListItem* list_item = static_cast<FSPanelCOFWearableOutfitListItem*>(panel);
            list_item->updateItemWeight(it->second);
        }
    }

    std::vector<LLPanel*> items;
    getItems(items);
    for (std::vector<LLPanel*>::const_iterator it = items.begin(); it != items.end(); ++it)
    {
        FSPanelCOFWearableOutfitListItem* list_item = static_cast<FSPanelCOFWearableOutfitListItem*>(*it);
        if (list_item->getWearableType() == LLWearableType::WT_SKIN)
        {
            list_item->updateItemWeight(mBodyPartsComplexity);
            break;
        }
    }
}
// </FS:Ansariel>

//////////////////////////////////////////////////////////////////////////
/// ContextMenu
//////////////////////////////////////////////////////////////////////////

LLWearableItemsList::ContextMenu::ContextMenu()
:   mParent(NULL)
{
}

void LLWearableItemsList::ContextMenu::show(LLView* spawning_view, const uuid_vec_t& uuids, S32 x, S32 y)
{
    mParent = dynamic_cast<LLWearableItemsList*>(spawning_view);
    LLListContextMenu::show(spawning_view, uuids, x, y);
    mParent = NULL; // to avoid dereferencing an invalid pointer
}

void LLWearableItemsList::ContextMenu::show(LLView* spawning_view, LLWearableType::EType w_type, S32 x, S32 y)
{
    mParent = dynamic_cast<LLWearableItemsList*>(spawning_view);
    LLContextMenu* menup = mMenuHandle.get();
    if (menup)
    {
        //preventing parent (menu holder) from deleting already "dead" context menus on exit
        LLView* parent = menup->getParent();
        if (parent)
        {
            parent->removeChild(menup);
        }
        delete menup;
        mUUIDs.clear();
    }

    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    registrar.add("Wearable.CreateNew", boost::bind(createNewWearableByType, w_type));
    menup = createFromFile("menu_wearable_list_item.xml");
    if (!menup)
    {
        LL_WARNS() << "Context menu creation failed" << LL_ENDL;
        return;
    }
    setMenuItemVisible(menup, "create_new", true);
    setMenuItemEnabled(menup, "create_new", true);
    setMenuItemVisible(menup, "wearable_attach_to", false);
    setMenuItemVisible(menup, "wearable_attach_to_hud", false);

    std::string new_label = LLTrans::getString("create_new_" + LLWearableType::getInstance()->getTypeName(w_type));
    LLMenuItemGL* menu_item = menup->getChild<LLMenuItemGL>("create_new");
    menu_item->setLabel(new_label);

    mMenuHandle = menup->getHandle();
    menup->show(x, y);
    LLMenuGL::showPopup(spawning_view, menup, x, y);

    mParent = NULL; // to avoid dereferencing an invalid pointer
}

// virtual
LLContextMenu* LLWearableItemsList::ContextMenu::createMenu()
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    const uuid_vec_t& ids = mUUIDs;     // selected items IDs
    LLUUID selected_id = ids.front();   // ID of the first selected item

    // Register handlers common for all wearable types.
    registrar.add("Wearable.Wear", boost::bind(wear_multiple, ids, true));
    registrar.add("Wearable.Add", boost::bind(wear_multiple, ids, false));
    registrar.add("Wearable.Edit", boost::bind(handle_item_edit, selected_id));
    registrar.add("Wearable.CreateNew", boost::bind(createNewWearable, selected_id));
    registrar.add("Wearable.ShowOriginal", boost::bind(show_item_original, selected_id));
    // <AS:Chanayane> Replace Links context menu entry
    registrar.add("Wearable.ReplaceLinks", boost::bind(replace_links, selected_id));
    // </AS:Chanayane>
    // <AS:Chanayane> Delete from outfit context menu entry
    registrar.add("Wearable.DeleteFromOutfit", boost::bind(delete_from_outfit, ids));
    // </AS:Chanayane>
    registrar.add("Wearable.TakeOffDetach",
                  //boost::bind(&LLAppearanceMgr::removeItemsFromAvatar, LLAppearanceMgr::getInstance(), ids, no_op)); // <FS:Ansariel> [SL:KB] - Patch: Appearance-Misc
                  boost::bind(&LLAppearanceMgr::removeItemsFromAvatar, LLAppearanceMgr::getInstance(), ids));

    // Register handlers for clothing.
    registrar.add("Clothing.TakeOff",
                  //boost::bind(&LLAppearanceMgr::removeItemsFromAvatar, LLAppearanceMgr::getInstance(), ids, no_op)); // <FS:Ansariel> [SL:KB] - Patch: Appearance-Misc
                  boost::bind(&LLAppearanceMgr::removeItemsFromAvatar, LLAppearanceMgr::getInstance(), ids));

    // Register handlers for body parts.

    // Register handlers for attachments.
    registrar.add("Attachment.Detach",
                  //boost::bind(&LLAppearanceMgr::removeItemsFromAvatar, LLAppearanceMgr::getInstance(), ids, no_op)); // <FS:Ansariel> [SL:KB] - Patch: Appearance-Misc
                  boost::bind(&LLAppearanceMgr::removeItemsFromAvatar, LLAppearanceMgr::getInstance(), ids));
    registrar.add("Attachment.Touch", boost::bind(handle_attachment_touch, selected_id));
    registrar.add("Attachment.Profile", boost::bind(show_item_profile, selected_id));
    registrar.add("Object.Attach", boost::bind(LLViewerAttachMenu::attachObjects, ids, _2));

    // Create the menu.
    LLContextMenu* menu = createFromFile("menu_wearable_list_item.xml");

    // Determine which items should be visible/enabled.
    updateItemsVisibility(menu);

    // Update labels for the items requiring that.
    updateItemsLabels(menu);
    return menu;
}

void LLWearableItemsList::ContextMenu::updateItemsVisibility(LLContextMenu* menu)
{
    if (!menu)
    {
        LL_WARNS() << "Invalid menu" << LL_ENDL;
        return;
    }

    const uuid_vec_t& ids = mUUIDs;             // selected items IDs
    U32 mask = 0;                               // mask of selected items' types
    U32 n_items = static_cast<U32>(ids.size()); // number of selected items
    U32 n_worn = 0;                             // number of worn items among the selected ones
    U32 n_already_worn = 0;                     // number of items worn of same type as selected items
    U32 n_links = 0;                            // number of links among the selected items
    U32 n_editable = 0;                         // number of editable items among the selected ones
    U32 n_touchable = 0;                        // number of touchable items among the selected ones

    bool can_be_worn = true;

// [RLVa:KB] - Checked: 2010-09-04 (RLVa-1.2.1a) | Added: RLVa-1.2.1a
    // We'll enable a menu option if at least one item in the selection is wearable/removable
    bool rlvCanWearReplace = !RlvActions::isRlvEnabled();
    bool rlvCanWearAdd = !RlvActions::isRlvEnabled();
    bool rlvCanRemove = !RlvActions::isRlvEnabled();
// [/RLVa:KB]

    for (uuid_vec_t::const_iterator it = ids.begin(); it != ids.end(); ++it)
    {
        LLUUID id = *it;
        LLViewerInventoryItem* item = gInventory.getItem(id);

        if (!item)
        {
            LL_WARNS() << "Invalid item" << LL_ENDL;
            // *NOTE: the logic below may not work in this case
            continue;
        }

        updateMask(mask, item->getType());

        const LLWearableType::EType wearable_type = item->getWearableType();
        const bool is_link = item->getIsLinkType();
        const bool is_worn = get_is_item_worn(id);
        const bool is_editable = get_is_item_editable(id);
        const bool is_touchable = enable_attachment_touch(id);
        const bool is_already_worn = gAgentWearables.selfHasWearable(wearable_type);
        if (is_worn)
        {
            ++n_worn;
        }
        if (is_touchable)
        {
            ++n_touchable;
        }
        if (is_editable)
        {
            ++n_editable;
        }
        if (is_link)
        {
            ++n_links;
        }
        if (is_already_worn)
        {
            ++n_already_worn;
        }

        if (can_be_worn)
        {
            can_be_worn = get_can_item_be_worn(item->getLinkedUUID());
        }

// [RLVa:KB] - Checked: 2010-09-04 (RLVa-1.2.1a) | Added: RLVa-1.2.1a
        if (RlvActions::isRlvEnabled())
        {
            ERlvWearMask eWearMask = RLV_WEAR_LOCKED;
            switch (item->getType())
            {
                case LLAssetType::AT_BODYPART:
                case LLAssetType::AT_CLOTHING:
                    eWearMask = gRlvWearableLocks.canWear(item);
                    rlvCanRemove |= (is_worn) ? gRlvWearableLocks.canRemove(item) : false;
                    break;
                case LLAssetType::AT_OBJECT:
                    eWearMask = gRlvAttachmentLocks.canAttach(item);
                    rlvCanRemove |= (is_worn) ? gRlvAttachmentLocks.canDetach(item) : false;
                    break;
                default:
                    break;
            }
            rlvCanWearReplace |= ((eWearMask & RLV_WEAR_REPLACE) == RLV_WEAR_REPLACE);
            rlvCanWearAdd |= ((eWearMask & RLV_WEAR_ADD) == RLV_WEAR_ADD);
        }
// [/RLVa:KB]
    } // for

    // <FS:Ansariel> Standalone check doesn't make sense here as the context
    //               menu is only shown if standalone is true. If not, this
    //               method isn't called at all and it is assumed you provide
    //               your own right-click handler (LLWearableItemsList::ContextMenu
    //               is only used in LLWearableItemsList::onRightClick handler
    //               method which in return is only set as event handler if
    //               standalone is true).
    bool standalone = /*mParent ? mParent->isStandalone() :*/ false;
    bool wear_add_visible = mask & (MASK_CLOTHING|MASK_ATTACHMENT) && n_worn == 0 && can_be_worn && (n_already_worn != 0 || mask & MASK_ATTACHMENT);
    bool show_create_new = mParent ? mParent->showCreateNew() : true; // <FS:Ansariel> Optional "Create new" menu item

    // *TODO: eliminate multiple traversals over the menu items
    setMenuItemVisible(menu, "wear_wear",           n_already_worn == 0 && n_worn == 0 && can_be_worn);
//  setMenuItemEnabled(menu, "wear_wear",           n_already_worn == 0 && n_worn == 0);
    setMenuItemVisible(menu, "wear_add",            wear_add_visible);
//  setMenuItemEnabled(menu, "wear_add",            LLAppearanceMgr::instance().canAddWearables(ids));
    setMenuItemVisible(menu, "wear_replace",        n_worn == 0 && n_already_worn != 0 && can_be_worn);
// [RLVa:KB] - Checked: 2010-09-04 (RLVa-1.2.1a) | Added: RLVa-1.2.1a
    setMenuItemEnabled(menu, "wear_wear",           n_already_worn == 0 && n_worn == 0 && rlvCanWearReplace);
    setMenuItemEnabled(menu, "wear_add",            LLAppearanceMgr::instance().canAddWearables(ids) && rlvCanWearAdd);
    setMenuItemEnabled(menu, "wear_replace",        rlvCanWearReplace);
// [/RLVa:KB]
    //visible only when one item selected and this item is worn
    setMenuItemVisible(menu, "touch",               !standalone && mask == MASK_ATTACHMENT && n_worn == n_items);
    setMenuItemEnabled(menu, "touch",               n_touchable && n_worn == 1 && n_items == 1);
    setMenuItemVisible(menu, "edit",                !standalone && mask & (MASK_CLOTHING|MASK_BODYPART|MASK_ATTACHMENT) && n_worn == n_items);
    setMenuItemEnabled(menu, "edit",                n_editable && n_worn == 1 && n_items == 1);
    // <FS:Ansariel> Optional "Create new" menu item
    //setMenuItemVisible(menu, "create_new",            mask & (MASK_CLOTHING|MASK_BODYPART) && n_items == 1);
    setMenuItemVisible(menu, "create_new",          show_create_new && mask & (MASK_CLOTHING|MASK_BODYPART) && n_items == 1);
    // </FS:Ansariel>
    setMenuItemEnabled(menu, "create_new",          LLAppearanceMgr::instance().canAddWearables(ids));
    setMenuItemVisible(menu, "show_original",       !standalone);
    setMenuItemEnabled(menu, "show_original",       n_items == 1 && n_links == n_items);
// <AS:Chanayane> Replace Links context menu entry
    setMenuItemVisible(menu, "replace_links",       n_links >= 1);
    setMenuItemEnabled(menu, "replace_links",       n_links == 1);
// </AS:Chanayane>
// <AS:Chanayane> Delete from outfit context menu entry
    setMenuItemVisible(menu, "delete_from_outfit",  n_links > 0);
    setMenuItemEnabled(menu, "delete_from_outfit",  n_links > 0);
// </AS:Chanayane>
    setMenuItemVisible(menu, "take_off",            mask == MASK_CLOTHING && n_worn == n_items);
    setMenuItemVisible(menu, "detach",              mask == MASK_ATTACHMENT && n_worn == n_items);
    setMenuItemVisible(menu, "take_off_or_detach",  mask == (MASK_ATTACHMENT|MASK_CLOTHING));
//  setMenuItemEnabled(menu, "take_off_or_detach",  n_worn == n_items);
// [RLVa:KB] - Checked: 2010-09-04 (RLVa-1.2.1a) | Added: RLVa-1.2.1a
    setMenuItemEnabled(menu, "take_off",            rlvCanRemove);
    setMenuItemEnabled(menu, "detach",              rlvCanRemove);
    setMenuItemEnabled(menu, "take_off_or_detach",  (n_worn == n_items) && (rlvCanRemove));
// [/RLVa:KB]
    setMenuItemVisible(menu, "object_profile",      !standalone);
    setMenuItemEnabled(menu, "object_profile",      n_items == 1);
    setMenuItemVisible(menu, "--no options--",      false);
    setMenuItemEnabled(menu, "--no options--",      false);

    // Populate or hide the "Attach to..." / "Attach to HUD..." submenus.
    if (mask == MASK_ATTACHMENT && n_worn == 0)
    {
        LLViewerAttachMenu::populateMenus("wearable_attach_to", "wearable_attach_to_hud");
    }
    else
    {
        setMenuItemVisible(menu, "wearable_attach_to",          false);
        setMenuItemVisible(menu, "wearable_attach_to_hud",      false);
    }

    if (mask & MASK_UNKNOWN)
    {
        LL_WARNS() << "Non-wearable items passed." << LL_ENDL;
    }

    U32 num_visible_items = 0;
    for (U32 menu_item_index = 0; menu_item_index < menu->getItemCount(); ++menu_item_index)
    {
        const LLMenuItemGL* menu_item = menu->getItem(menu_item_index);
        if (menu_item && menu_item->getVisible())
        {
            num_visible_items++;
        }
    }
    if (num_visible_items == 0)
    {
        setMenuItemVisible(menu, "--no options--", true);
    }
}

void LLWearableItemsList::ContextMenu::updateItemsLabels(LLContextMenu* menu)
{
    llassert(menu);
    if (!menu) return;

    // Set proper label for the "Create new <WEARABLE_TYPE>" menu item.
    LLViewerInventoryItem* item = gInventory.getLinkedItem(mUUIDs.back());
    if (!item || !item->isWearableType()) return;

    LLWearableType::EType w_type = item->getWearableType();
    std::string new_label = LLTrans::getString("create_new_" + LLWearableType::getInstance()->getTypeName(w_type));

    LLMenuItemGL* menu_item = menu->getChild<LLMenuItemGL>("create_new");
    menu_item->setLabel(new_label);
}

// We need this method to convert non-zero bool values to exactly 1 (true).
// Otherwise code relying on a bool value being true may fail
// (I experienced a weird assert in LLView::drawChildren() because of that.
// static
void LLWearableItemsList::ContextMenu::setMenuItemVisible(LLContextMenu* menu, const std::string& name, bool val)
{
    menu->setItemVisible(name, val);
}

// static
void LLWearableItemsList::ContextMenu::setMenuItemEnabled(LLContextMenu* menu, const std::string& name, bool val)
{
    menu->setItemEnabled(name, val);
}

// static
void LLWearableItemsList::ContextMenu::updateMask(U32& mask, LLAssetType::EType at)
{
    if (at == LLAssetType::AT_CLOTHING)
    {
        mask |= MASK_CLOTHING;
    }
    else if (at == LLAssetType::AT_BODYPART)
    {
        mask |= MASK_BODYPART;
    }
    else if (at == LLAssetType::AT_OBJECT)
    {
        mask |= MASK_ATTACHMENT;
    }
    else if (at == LLAssetType::AT_GESTURE)
    {
        mask |= MASK_GESTURE;
    }
    else
    {
        mask |= MASK_UNKNOWN;
    }
}

// static
void LLWearableItemsList::ContextMenu::createNewWearable(const LLUUID& item_id)
{
    LLViewerInventoryItem* item = gInventory.getLinkedItem(item_id);
    if (!item || !item->isWearableType()) return;

    LLAgentWearables::createWearable(item->getWearableType(), true);
}

// static
void LLWearableItemsList::ContextMenu::createNewWearableByType(LLWearableType::EType type)
{
    LLAgentWearables::createWearable(type, true);
}

// EOF
