/*
 *  SPDX-FileCopyrightText: 2007 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2008 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "kis_node_model.h"

#include <iostream>

#include <QMimeData>
#include <QBuffer>
#include <QPointer>

#include <KoColorSpaceConstants.h>
#include <KoCompositeOpRegistry.h>

#include <klocalizedstring.h>

#include "kis_mimedata.h"
#include <kis_debug.h>
#include <kis_node.h>
#include <kis_node_progress_proxy.h>
#include <kis_image.h>
#include <kis_selection.h>
#include <kis_selection_mask.h>
#include <kis_undo_adapter.h>
#include <commands/kis_node_property_list_command.h>
#include <kis_paint_layer.h>
#include <kis_group_layer.h>
#include <kis_projection_leaf.h>
#include <kis_shape_controller.h>

#include "kis_dummies_facade_base.h"
#include "kis_filter_mask.h"
#include "kis_node_dummies_graph.h"
#include "kis_model_index_converter.h"
#include "kis_model_index_converter_show_all.h"
#include "kis_node_selection_adapter.h"
#include "kis_node_insertion_adapter.h"
#include "kis_node_manager.h"
#include <KisSelectionActionsAdapter.h>
#include <KisNodeDisplayModeAdapter.h>

#include "kis_config.h"
#include "kis_config_notifier.h"
#include "kis_signal_auto_connection.h"
#include "kis_signal_compressor.h"
#include "KisLayerThumbnailCache.h"


struct KisNodeModel::Private
{
    Private() : updateCompressor(100, KisSignalCompressor::FIRST_ACTIVE) {}

    KisImageWSP image;
    KisShapeController *shapeController = 0;
    KisNodeSelectionAdapter *nodeSelectionAdapter = 0;
    KisNodeInsertionAdapter *nodeInsertionAdapter = 0;
    KisSelectionActionsAdapter *selectionActionsAdapter = 0;
    KisNodeDisplayModeAdapter *nodeDisplayModeAdapter = 0;
    KisNodeManager *nodeManager = 0;

    KisSignalAutoConnectionsStore nodeDisplayModeAdapterConnections;

    QList<KisNodeDummy*> updateQueue;
    KisSignalCompressor updateCompressor;

    KisModelIndexConverterBase *indexConverter = 0;
    QPointer<KisDummiesFacadeBase> dummiesFacade = 0;
    bool needFinishRemoveRows = false;
    bool needFinishInsertRows = false;
    bool showRootLayer = false;
    bool showGlobalSelection = false;
    int dummyColumns {0};
    QPersistentModelIndex activeNodeIndex;

    QPointer<KisNodeDummy> parentOfRemovedNode = 0;

    QSet<quintptr> dropEnabled;

    KisLayerThumbnailCache thumbnalCache;
};

KisNodeModel::KisNodeModel(QObject * parent, int clonedColumns)
        : QAbstractItemModel(parent)
        , m_d(new Private)
{
    m_d->dummyColumns = qMax(0, clonedColumns);
    connect(&m_d->updateCompressor, SIGNAL(timeout()), SLOT(processUpdateQueue()));
    connect(&m_d->thumbnalCache, SIGNAL(sigLayerThumbnailUpdated(KisNodeSP)), SLOT(slotLayerThumbnailUpdated(KisNodeSP)));
}

KisNodeModel::~KisNodeModel()
{
    delete m_d->indexConverter;
    delete m_d;
}

KisNodeSP KisNodeModel::nodeFromIndex(const QModelIndex &index) const
{
    Q_ASSERT(index.isValid());

    KisNodeDummy *dummy = m_d->indexConverter->dummyFromIndex(index);
    if (dummy) {
        return dummy->node();
    }
    return 0;
}

QModelIndex KisNodeModel::indexFromNode(KisNodeSP node) const
{
    KisNodeDummy *dummy = m_d->dummiesFacade->dummyForNode(node);
    if(dummy)
        return m_d->indexConverter->indexFromDummy(dummy);
    return QModelIndex();
}

bool KisNodeModel::belongsToIsolatedGroup(KisImageSP image, KisNodeSP node, KisDummiesFacadeBase *dummiesFacade)
{
    KisNodeSP isolatedRoot = image->isolationRootNode();
    if (!isolatedRoot) return true;

    KisNodeDummy *isolatedRootDummy =
        dummiesFacade->dummyForNode(isolatedRoot);
    KisNodeDummy *dummy =
        dummiesFacade->dummyForNode(node);

    while (dummy) {
        if (dummy == isolatedRootDummy) {
            return true;
        }
        dummy = dummy->parent();
    }

    return false;
}

bool KisNodeModel::belongsToIsolatedGroup(KisNodeSP node) const
{
    return belongsToIsolatedGroup(m_d->image, node, m_d->dummiesFacade);
}

void KisNodeModel::resetIndexConverter()
{
    delete m_d->indexConverter;
    m_d->indexConverter = 0;

    if(m_d->dummiesFacade) {
        m_d->indexConverter = createIndexConverter();
    }
}

KisModelIndexConverterBase *KisNodeModel::createIndexConverter()
{
    if(m_d->showRootLayer) {
        return new KisModelIndexConverterShowAll(m_d->dummiesFacade, this);
    } else {
        return new KisModelIndexConverter(m_d->dummiesFacade, this, m_d->showGlobalSelection);
    }
}

void KisNodeModel::regenerateItems(KisNodeDummy *dummy)
{
    const QModelIndex &index = m_d->indexConverter->indexFromDummy(dummy);
    Q_EMIT dataChanged(index.siblingAtColumn(0), index.siblingAtColumn(m_d->dummyColumns));

    dummy = dummy->firstChild();
    while (dummy) {
        regenerateItems(dummy);
        dummy = dummy->nextSibling();
    }
}

void KisNodeModel::slotIsolatedModeChanged()
{
    KisNodeDummy *rootDummy = m_d->dummiesFacade->rootDummy();
    if (!rootDummy) return;

    regenerateItems(rootDummy);
}

bool KisNodeModel::showGlobalSelection() const
{
    return m_d->nodeDisplayModeAdapter ?
        m_d->nodeDisplayModeAdapter->showGlobalSelectionMask() :
        false;
}

void KisNodeModel::setPreferredThumnalSize(int preferredSize) const
{
    m_d->thumbnalCache.setMaxSize(preferredSize);
}

void KisNodeModel::setShowGlobalSelection(bool value)
{
    if (m_d->nodeDisplayModeAdapter) {
        m_d->nodeDisplayModeAdapter->setShowGlobalSelectionMask(value);
    }
}

void KisNodeModel::slotNodeDisplayModeChanged(bool showRootNode, bool showGlobalSelectionMask)
{
    const bool oldShowRootLayer = m_d->showRootLayer;
    const bool oldShowGlobalSelection = m_d->showGlobalSelection;
    m_d->showRootLayer = showRootNode;
    m_d->showGlobalSelection = showGlobalSelectionMask;

    if (m_d->showRootLayer != oldShowRootLayer || m_d->showGlobalSelection != oldShowGlobalSelection) {
        resetIndexConverter();
        beginResetModel();
        endResetModel();
    }
}

void KisNodeModel::progressPercentageChanged(int, const KisNodeSP node)
{
    if(!m_d->dummiesFacade) return;

    // Need to check here as the node might already be removed, but there might
    // still be some signals arriving from another thread
    if (m_d->dummiesFacade->hasDummyForNode(node)) {
        QModelIndex index = indexFromNode(node);

        Q_EMIT dataChanged(index, index);
    }
}

void KisNodeModel::slotLayerThumbnailUpdated(KisNodeSP node)
{
    QModelIndex index = indexFromNode(node);
    if (!index.isValid()) return;

    Q_EMIT dataChanged(index, index);
}

KisModelIndexConverterBase * KisNodeModel::indexConverter() const
{
    return m_d->indexConverter;
}

KisDummiesFacadeBase *KisNodeModel::dummiesFacade() const
{
    return m_d->dummiesFacade;
}

void KisNodeModel::connectDummy(KisNodeDummy *dummy, bool needConnect)
{
    KisNodeSP node = dummy->node();
    if (!node) {
        qWarning() << "Dummy node has no node!" << dummy << dummy->node();
        return;
    }
    KisNodeProgressProxy *progressProxy = node->nodeProgressProxy();
    if(progressProxy) {
        if(needConnect) {
            connect(progressProxy, SIGNAL(percentageChanged(int,KisNodeSP)),
                    SLOT(progressPercentageChanged(int,KisNodeSP)));
        } else {
            progressProxy->disconnect(this);
        }
    }
}

void KisNodeModel::connectDummies(KisNodeDummy *dummy, bool needConnect)
{
    connectDummy(dummy, needConnect);

    dummy = dummy->firstChild();
    while(dummy) {
        connectDummies(dummy, needConnect);
        dummy = dummy->nextSibling();
    }
}

void KisNodeModel::setDummiesFacade(KisDummiesFacadeBase *dummiesFacade,
                                    KisImageWSP image,
                                    KisShapeController *shapeController,
                                    KisSelectionActionsAdapter *selectionActionsAdapter,
                                    KisNodeManager *nodeManager)
{
    QPointer<KisDummiesFacadeBase> oldDummiesFacade(m_d->dummiesFacade);
    KisShapeController  *oldShapeController = m_d->shapeController;

    m_d->shapeController = shapeController;
    m_d->nodeManager = nodeManager;
    m_d->nodeSelectionAdapter = nodeManager ? nodeManager->nodeSelectionAdapter() : nullptr;
    m_d->nodeInsertionAdapter = nodeManager ? nodeManager->nodeInsertionAdapter() : nullptr;
    m_d->selectionActionsAdapter = selectionActionsAdapter;

    m_d->nodeDisplayModeAdapterConnections.clear();
    m_d->nodeDisplayModeAdapter = nodeManager ? nodeManager->nodeDisplayModeAdapter() : nullptr;
    if (m_d->nodeDisplayModeAdapter) {
        m_d->nodeDisplayModeAdapterConnections.addConnection(
            m_d->nodeDisplayModeAdapter, SIGNAL(sigNodeDisplayModeChanged(bool,bool)),
            this, SLOT(slotNodeDisplayModeChanged(bool,bool)));

        // cold initialization
        m_d->showGlobalSelection = m_d->nodeDisplayModeAdapter->showGlobalSelectionMask();
        m_d->showRootLayer = false;
    }

    if (oldDummiesFacade && m_d->image) {
        m_d->image->disconnect(this);
        oldDummiesFacade->disconnect(this);
        KisNodeDummy *oldRootDummy = m_d->dummiesFacade->rootDummy();
        if (oldRootDummy) {
            connectDummies(oldRootDummy, false);
        }
    }

    m_d->image = image;
    m_d->dummiesFacade = dummiesFacade;
    m_d->parentOfRemovedNode = 0;
    m_d->thumbnalCache.setImage(image);
    resetIndexConverter();

    if (m_d->dummiesFacade) {
        KisNodeDummy *rootDummy = m_d->dummiesFacade->rootDummy();
        if (rootDummy) {
            connectDummies(rootDummy, true);
        }

        connect(m_d->dummiesFacade, SIGNAL(sigBeginInsertDummy(KisNodeDummy*,int,QString)),
                SLOT(slotBeginInsertDummy(KisNodeDummy*,int,QString)));
        connect(m_d->dummiesFacade, SIGNAL(sigEndInsertDummy(KisNodeDummy*)),
                SLOT(slotEndInsertDummy(KisNodeDummy*)));
        connect(m_d->dummiesFacade, SIGNAL(sigBeginRemoveDummy(KisNodeDummy*)),
                SLOT(slotBeginRemoveDummy(KisNodeDummy*)));
        connect(m_d->dummiesFacade, SIGNAL(sigEndRemoveDummy()),
                SLOT(slotEndRemoveDummy()));

        connect(m_d->dummiesFacade, SIGNAL(sigDummyChanged(KisNodeDummy*)),
                SLOT(slotDummyChanged(KisNodeDummy*)));

        if (m_d->image.isValid()) {
            connect(m_d->image, SIGNAL(sigIsolatedModeChanged()), SLOT(slotIsolatedModeChanged()));
        }
    }

    if (m_d->dummiesFacade != oldDummiesFacade || m_d->shapeController != oldShapeController) {
        beginResetModel();
        endResetModel();
    }
}

void KisNodeModel::setIdleTaskManager(KisIdleTasksManager *idleTasksManager)
{
    m_d->thumbnalCache.setIdleTaskManager(idleTasksManager);
}

void KisNodeModel::slotBeginInsertDummy(KisNodeDummy *parent, int index, const QString &metaObjectType)
{
    int row = 0;
    QModelIndex parentIndex;

    bool willAdd =
        m_d->indexConverter->indexFromAddedDummy(parent, index,
                                                 metaObjectType,
                                                 parentIndex, row);

    if(willAdd) {
        beginInsertRows(parentIndex, row, row);
        m_d->needFinishInsertRows = true;
    }
}

void KisNodeModel::slotEndInsertDummy(KisNodeDummy *dummy)
{
    if(m_d->needFinishInsertRows) {
        connectDummy(dummy, true);
        endInsertRows();
        m_d->needFinishInsertRows = false;
    }

    m_d->thumbnalCache.notifyNodeAdded(dummy->node());
}

void KisNodeModel::slotBeginRemoveDummy(KisNodeDummy *dummy)
{
    if (!dummy) return;

    // FIXME: is it really what we want?
    m_d->updateCompressor.stop();
    m_d->updateQueue.clear();

    m_d->parentOfRemovedNode = dummy->parent();

    QModelIndex parentIndex;
    if (m_d->parentOfRemovedNode) {
        parentIndex = m_d->indexConverter->indexFromDummy(m_d->parentOfRemovedNode);
    }

    QModelIndex itemIndex = m_d->indexConverter->indexFromDummy(dummy);

    if (itemIndex.isValid()) {
        connectDummy(dummy, false);
        Q_EMIT sigBeforeBeginRemoveRows(parentIndex, itemIndex.row(), itemIndex.row());
        beginRemoveRows(parentIndex, itemIndex.row(), itemIndex.row());
        m_d->needFinishRemoveRows = true;
    }

    m_d->thumbnalCache.notifyNodeRemoved(dummy->node());
}

void KisNodeModel::slotEndRemoveDummy()
{
    if(m_d->needFinishRemoveRows) {
        endRemoveRows();
        m_d->needFinishRemoveRows = false;
    }
}

void KisNodeModel::slotDummyChanged(KisNodeDummy *dummy)
{
    if (!m_d->updateQueue.contains(dummy)) {
        m_d->updateQueue.append(dummy);
    }
    m_d->updateCompressor.start();
}

void addChangedIndex(const QModelIndex &idx, QSet<QModelIndex> *indexes)
{
    if (!idx.isValid() || indexes->contains(idx)) return;

    indexes->insert(idx);

    const int rowCount = idx.model()->rowCount(idx);
    for (int i = 0; i < rowCount; i++) {
        addChangedIndex(idx.model()->index(i, 0, idx), indexes);
    }
}


void KisNodeModel::processUpdateQueue()
{
    QSet<QModelIndex> indexes;

    Q_FOREACH (KisNodeDummy *dummy, m_d->updateQueue) {
        QModelIndex index = m_d->indexConverter->indexFromDummy(dummy);
        addChangedIndex(index, &indexes);
    }

    Q_FOREACH (const QModelIndex &index, indexes) {
        Q_EMIT dataChanged(index.siblingAtColumn(0), index.siblingAtColumn(m_d->dummyColumns));
    }

    m_d->updateQueue.clear();
}

QModelIndex KisNodeModel::index(int row, int col, const QModelIndex &parent) const
{
    if(!m_d->dummiesFacade || !hasIndex(row, col, parent)) return QModelIndex();

    QModelIndex itemIndex;

    KisNodeDummy *dummy = m_d->indexConverter->dummyFromRow(row, parent);
    if(dummy) {
        itemIndex = m_d->indexConverter->indexFromDummy(dummy);
    }

    if (itemIndex.isValid() && itemIndex.column() != col) {
        itemIndex = createIndex(itemIndex.row(), col, itemIndex.internalPointer());
    }

    return itemIndex;
}

int KisNodeModel::rowCount(const QModelIndex &parent) const
{
    if(!m_d->dummiesFacade) return 0;
    if (parent.column() > 0) {
        return 0;
    }
    return m_d->indexConverter->rowCount(parent);
}

int KisNodeModel::columnCount(const QModelIndex &parent) const
{
    if (parent.column() > 0) {
        return 0;
    }
    return 1 + m_d->dummyColumns;
}

QModelIndex KisNodeModel::parent(const QModelIndex &index) const
{
    if(!m_d->dummiesFacade || !index.isValid()) return QModelIndex();

    KisNodeDummy *dummy = m_d->indexConverter->dummyFromIndex(index);
    KisNodeDummy *parentDummy = dummy->parent();

    QModelIndex parentIndex;

    if(parentDummy) {
        parentIndex = m_d->indexConverter->indexFromDummy(parentDummy);
    }

    return parentIndex;
}

QModelIndex KisNodeModel::sibling(int row, int column, const QModelIndex &idx) const
{
    // if it's just a different clone column, there's no need to lookup anything
    if (row == idx.row()) {
        if (column == idx.column()) {
            return idx;
        }
        KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(idx.model() == this, QModelIndex());
        return createIndex(row, column, idx.internalPointer());
    }
    return index(row, column, parent(idx));
}

QVariant KisNodeModel::data(const QModelIndex &index, int role) const
{
    if (!m_d->dummiesFacade || !index.isValid() || !m_d->image.isValid()) return QVariant();

    KisNodeSP node = nodeFromIndex(index);

    switch (role) {
    case Qt::DisplayRole: return node->name();
    case Qt::DecorationRole: return node->icon();
    case Qt::EditRole: return node->name();
    case Qt::SizeHintRole: return m_d->image->size(); // FIXME
    case Qt::ForegroundRole:
        return belongsToIsolatedGroup(node) &&
            !node->projectionLeaf()->isDroppedNode() ? QVariant() : QVariant(QColor(Qt::gray));
    case Qt::FontRole: {
        QFont baseFont;
        if (node->projectionLeaf()->isDroppedNode()) {
            baseFont.setStrikeOut(true);
        }
        if (m_d->activeNodeIndex == index) {
            baseFont.setBold(true);
        }
        return baseFont;
    }
    case KisNodeModel::PropertiesRole: return QVariant::fromValue(node->sectionModelProperties());
    case KisNodeModel::AspectRatioRole: return double(m_d->image->width()) / m_d->image->height();
    case KisNodeModel::ProgressRole: {
        KisNodeProgressProxy *proxy = node->nodeProgressProxy();
        return proxy ? proxy->percentage() : -1;
    }
    case KisNodeModel::ActiveRole: {
        return m_d->activeNodeIndex == index;
    }
    case KisNodeModel::ShouldGrayOutRole: {
        return !node->visible(true);
    }
    case KisNodeModel::ColorLabelIndexRole: {
        return node->colorLabelIndex();
    }
    case KisNodeModel::DropReasonRole: {
        QString result;
        KisProjectionLeaf::NodeDropReason reason = node->projectionLeaf()->dropReason();

        if (reason == KisProjectionLeaf::DropPassThroughMask) {
            result = i18nc("@info:tooltip", "Disabled: masks on pass-through groups are not supported!");
        } else if (reason == KisProjectionLeaf::DropPassThroughClone) {
            result = i18nc("@info:tooltip", "Disabled: cloning pass-through groups is not supported!");
        }

        return result;
    }
    case KisNodeModel::IsAnimatedRole: {
        return node->isAnimated();
    }
    case KisNodeModel::InfoTextRole: {
        // These layer types' opacity and blending modes cannot be changed,
        // so there's little point in showing them
        if (node->inherits("KisFilterMask") ||
            node->inherits("KisTransparencyMask") ||
            node->inherits("KisTransformMask") ||
            node->inherits("KisSelectionMask")) {
            return "";
        }
        const KisConfig::LayerInfoTextStyle infoTextStyle = KisConfig(true).layerInfoTextStyle();
        const int opacity = round(node->opacity() * 100.0 / 255);
        const QString opacityString = QString::number(opacity);
        const QString compositeOpId = node->compositeOpId();
        QString compositeOpDesc = "null";
        // make sure the compositeOp exists to avoid crashing on specific layer undo
        if (node->compositeOp()) {
            compositeOpDesc = node->compositeOp()->description();
        }
        QString defaultOpId = COMPOSITE_OVER;   // "normal";
        if (node->inherits("KisAdjustmentLayer")) {
            defaultOpId = COMPOSITE_COPY;
        }
        else if (node->inherits("KisColorizeMask")) {
            defaultOpId = COMPOSITE_BEHIND;
        }
        QString infoText = "";
        if (infoTextStyle == KisConfig::LayerInfoTextStyle::INFOTEXT_DETAILED ||
                !(opacity == 100 && compositeOpId == defaultOpId)) {
            if (infoTextStyle == KisConfig::LayerInfoTextStyle::INFOTEXT_SIMPLE) {
                if (opacity == 100) {
                    return QString(compositeOpDesc);
                }
                if (compositeOpId == defaultOpId) {
                    return i18nc("%1 is the percent value, % is the percent sign", "%1%", opacityString);
                }
            }
            infoText = i18nc("%1 is the percent value, % is the percent sign", "%1% %2", opacityString, compositeOpDesc);
        }
        return infoText;
    }
    case FilterMaskColorRole: {
        if (node->inherits("KisFilterMask")) {
            KisFilterMaskSP mask = qobject_cast<KisFilterMask*>(node.data());
            // The main use case is "fastcoloroverlay" filter, to display its color in the UI.
            if (mask->filter()->hasProperty("color")) {
                return mask->filter()->getColor("color").toQColor();
            }
        }
        return QVariant();
    }
    case LayerColorOverlayColorRole: {
        if (node->inherits("KisLayer")) {
            KisLayerSP layer = qobject_cast<KisLayer*>(node.data());
            KisFilterMaskSP mask = layer->colorOverlayMask();
            if (mask && mask->filter()->hasProperty("color")) {
                return mask->filter()->getColor("color").toQColor();
            }
        }
        return QVariant();
    }
    default:

        /**
         * The dummies are removed from the model asynchronously to the image operations,
         * therefore we should make sure that `node->graphListener()` is still valid and
         * this node is still present in the node graph.
         */
        if (role >= int(KisNodeModel::BeginThumbnailRole) &&
            belongsToIsolatedGroup(node) &&
            node->graphListener()) {

            /**
             * WARNING: there is still a possible theoretical race condition if the node is
             * removed from the image right here. We consider that as "improbable" atm.
             */

            const int maxSize = role - int(KisNodeModel::BeginThumbnailRole);

            if (maxSize == m_d->thumbnalCache.maxSize()) {
                return m_d->thumbnalCache.thumbnail(node);
            } else {
                return node->createThumbnail(maxSize, maxSize, Qt::KeepAspectRatio);
            }
        } else {
            return QVariant();
        }
    }

    return QVariant();
}

Qt::ItemFlags KisNodeModel::flags(const QModelIndex &index) const
{
    if(!m_d->dummiesFacade || !index.isValid()) return Qt::ItemIsDropEnabled;

    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsEditable;
    // currently dummy columns are neither selectable nor drag&drop enabled
    if (index.column() == 0) {
        flags |=  Qt::ItemIsDragEnabled | Qt::ItemIsSelectable;
        if (m_d->dropEnabled.contains(index.internalId())) {
            flags |= Qt::ItemIsDropEnabled;
        }

    }

    return flags;
}

bool KisNodeModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role == KisNodeModel::DropEnabled) {
        const QMimeData *mimeData = static_cast<const QMimeData*>(value.value<void*>());
        setDropEnabled(mimeData);
        return true;
    }

    if (role == KisNodeModel::ActiveRole || role == KisNodeModel::AlternateActiveRole) {
        QModelIndex parentIndex;
        if (!index.isValid() && m_d->parentOfRemovedNode && m_d->dummiesFacade && m_d->indexConverter) {
            parentIndex = m_d->indexConverter->indexFromDummy(m_d->parentOfRemovedNode);
            m_d->parentOfRemovedNode = 0;
        }

        KisNodeSP activatedNode;

        if (index.isValid() && value.toBool()) {
            activatedNode = nodeFromIndex(index);
        }
        else if (parentIndex.isValid() && value.toBool()) {
            activatedNode = nodeFromIndex(parentIndex);
        }
        else {
            activatedNode = 0;
        }

        QModelIndex newActiveNode = activatedNode ? indexFromNode(activatedNode) : QModelIndex();
        if (role == KisNodeModel::ActiveRole && value.toBool() &&
            m_d->activeNodeIndex == newActiveNode) {

            return true;
        }

        m_d->activeNodeIndex = newActiveNode;

        if (m_d->nodeSelectionAdapter) {
            m_d->nodeSelectionAdapter->setActiveNode(activatedNode);
        }

        if (role == KisNodeModel::AlternateActiveRole) {
            Q_EMIT toggleIsolateActiveNode();
        }

        Q_EMIT dataChanged(index.siblingAtColumn(0), index.siblingAtColumn(m_d->dummyColumns));
        return true;
    }

    if(!m_d->dummiesFacade || !index.isValid()) return false;

    bool result = true;
    bool shouldUpdate = true;
    bool shouldUpdateRecursively = false;
    KisNodeSP node = nodeFromIndex(index);

    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        m_d->nodeManager->setNodeName(node, value.toString());
        break;
    case KisNodeModel::PropertiesRole:
        {
            // don't record undo/redo for visibility, locked or alpha locked changes
            KisBaseNode::PropertyList proplist = value.value<KisBaseNode::PropertyList>();
            m_d->nodeManager->trySetNodeProperties(node, m_d->image, proplist);
            shouldUpdateRecursively = true;

            break;
        }
    case KisNodeModel::SelectOpaqueRole:
        if (node && m_d->selectionActionsAdapter) {
            SelectionAction action = SelectionAction(value.toInt());
            m_d->selectionActionsAdapter->selectOpaqueOnNode(node, action);
        }
        shouldUpdate = false;
        break;
    case FilterMaskPropertiesRole:
        m_d->nodeManager->nodePropertiesIgnoreSelection(node);
        break;
    case LayerColorOverlayPropertiesRole:
        m_d->nodeManager->colorOverlayMaskProperties(node);
        break;
    default:
        result = false;
    }

    if (result && shouldUpdate) {
        if (shouldUpdateRecursively) {
            QSet<QModelIndex> indexes;
            addChangedIndex(index, &indexes);
            Q_FOREACH (const QModelIndex &idx, indexes) {
                Q_EMIT dataChanged(idx.siblingAtColumn(0), idx.siblingAtColumn(m_d->dummyColumns));
            }
        } else {
            Q_EMIT dataChanged(index.siblingAtColumn(0), index.siblingAtColumn(m_d->dummyColumns));
        }
    }

    return result;
}

Qt::DropActions KisNodeModel::supportedDragActions() const
{
    return Qt::CopyAction | Qt::MoveAction;
}

Qt::DropActions KisNodeModel::supportedDropActions() const
{
    return Qt::MoveAction | Qt::CopyAction;
}

bool KisNodeModel::hasDummiesFacade()
{
    return m_d->dummiesFacade != 0;
}

QStringList KisNodeModel::mimeTypes() const
{
    QStringList types;
    types << QLatin1String("application/x-krita-node-internal-pointer");
    types << QLatin1String("application/x-qt-image");
    types << QLatin1String("application/x-color");
    types << QLatin1String("krita/x-colorsetentry");
    return types;
}

QMimeData * KisNodeModel::mimeData(const QModelIndexList &indexes) const
{
    bool hasLockedLayer = false;
    KisNodeList nodes;
    Q_FOREACH (const QModelIndex &idx, indexes) {
        // Although clone columns should not be selectable, make sure we only use column 0,
        // because nodeFromIndex doesn't like duplicate list entries.
        if (idx.column() != 0) {
            continue;
        }

        KisNodeSP node = nodeFromIndex(idx);

        nodes << node;
        hasLockedLayer |= !node->isEditable(false);
    }

    return KisMimeData::mimeForLayers(nodes, m_d->image, hasLockedLayer);
}

bool KisNodeModel::dropMimeData(const QMimeData * data, Qt::DropAction action, int row, int column, const QModelIndex & parent)
{
    Q_UNUSED(column);

    bool copyNode = (action == Qt::CopyAction);

    KisNodeDummy *parentDummy = 0;
    KisNodeDummy *aboveThisDummy = 0;

    parentDummy = parent.isValid() ?
        m_d->indexConverter->dummyFromIndex(parent) :
        m_d->dummiesFacade->rootDummy();

    if (row == -1) {
        aboveThisDummy = parent.isValid() ? parentDummy->lastChild() : 0;
    }
    else {
        aboveThisDummy = row < m_d->indexConverter->rowCount(parent) ? m_d->indexConverter->dummyFromRow(row, parent) : 0;
    }

    return KisMimeData::insertMimeLayers(data,
                                         m_d->image,
                                         m_d->shapeController,
                                         parentDummy,
                                         aboveThisDummy,
                                         copyNode,
                                         m_d->nodeInsertionAdapter);
}

bool KisNodeModel::canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) const {
    if (parent.isValid()) {
        // drop occurred on an item. always return true as returning false will mess up
        // QT5's drag handling (see KisNodeModel::setDropEnabled).
        return true;
    } else {
        return QAbstractItemModel::canDropMimeData(data, action, row, column, parent);
    }
}

void KisNodeModel::setDropEnabled(const QMimeData *data) {
    // what happens here should really happen in KisNodeModel::canDropMimeData(), but QT5
    // will mess up if an item's Qt::ItemIsDropEnabled does not match what is returned by
    // canDropMimeData; specifically, if we set the flag, but decide in canDropMimeData()
    // later on that an "onto" drag is not allowed, QT will display an drop indicator for
    // insertion, but not perform any drop when the mouse is released.

    // the only robust implementation seems to set all flags correctly, which is done here.

    bool copyNode = false;
    KisNodeList nodes = KisMimeData::loadNodesFast(data, m_d->image, m_d->shapeController, copyNode);
    m_d->dropEnabled.clear();
    updateDropEnabled(nodes);
}

void KisNodeModel::updateDropEnabled(const QList<KisNodeSP> &nodes, QModelIndex parent) {
    for (int r = 0; r < rowCount(parent); r++) {
        QModelIndex idx = index(r, 0, parent);

        KisNodeSP target = nodeFromIndex(idx);

        bool dropEnabled = true;
        Q_FOREACH (const KisNodeSP &node, nodes) {
            if (!target->allowAsChild(node) || !target->isEditable(false)) {
                dropEnabled = false;
                break;
            }
        }
        if (dropEnabled) {
            m_d->dropEnabled.insert(idx.internalId());
        }
        Q_EMIT dataChanged(idx, idx); // indicate to QT that flags() have changed

        if (hasChildren(idx)) {
            updateDropEnabled(nodes, idx);
        }
    }
}
