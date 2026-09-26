// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/layers_panel.hpp"

#include <QComboBox>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QScrollBar>
#include <QLabel>
#include <QMenu>
#include <QTreeWidgetItemIterator>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QSet>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

#include "core/adjust/adjustment.hpp"
#include "core/doc/blend_mode.hpp"
#include "gui/icons.hpp"
#include "gui/json_util.hpp"
#include "gui/op_runner.hpp"
#include "gui/session.hpp"
#include "gui/theme.hpp"

namespace rl::gui {

namespace {

constexpr int kColEye = 0, kColMain = 1, kColMask = 2, kColFlags = 3;
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kThumb = 30;

QString adjustment_display(const std::string& type) {
    static const std::pair<const char*, const char*> kNames[] = {
        {"levels", "Levels"}, {"curves", "Curves"}, {"brightness_contrast", "Brightness/Contrast"}, {"hue_saturation", "Hue/Saturation"},
        {"black_white", "Black & White"}, {"invert", "Invert"}, {"posterize", "Posterize"}, {"threshold", "Threshold"}};
    for (const auto& [t, n] : kNames)
        if (type == t) return QObject::tr(n);
    return QString::fromStdString(type);
}

const rl::Node* find_node(const rl::Node& c, const std::string& id, const rl::Node** parent = nullptr,
                          int* index = nullptr) {
    for (size_t i = 0; i < c.children.size(); ++i) {
        const rl::Node& n = c.children[i];
        if (n.id == id) {
            if (parent) *parent = &c;
            if (index) *index = static_cast<int>(i);
            return &n;
        }
        if (n.is_group())
            if (const rl::Node* r = find_node(n, id, parent, index)) return r;
    }
    return nullptr;
}

QString mode_display(rl::BlendMode m) {
    for (const auto& [name, json] : LayersPanel::blend_modes(true))
        if (json == QLatin1String(rl::blend_mode_name(m))) return name;
    return QString::fromLatin1(rl::blend_mode_name(m));
}

QPixmap checker_thumb() {
    QPixmap pm(kThumb, kThumb);
    QPainter p(&pm);
    for (int y = 0; y < kThumb; y += 5)
        for (int x = 0; x < kThumb; x += 5)
            p.fillRect(x, y, 5, 5, ((x + y) / 5) % 2 ? QColor(0x9a, 0x9a, 0x9a) : QColor(0xc8, 0xc8, 0xc8));
    return pm;
}

}  // namespace

QList<QPair<QString, QString>> LayersPanel::blend_modes(bool group) {
    QList<QPair<QString, QString>> m;
    if (group) {
        m << qMakePair(QObject::tr("Pass Through"), QStringLiteral("pass"));
    }
    m << qMakePair(QObject::tr("Normal"), QStringLiteral("norm")) << qMakePair(QObject::tr("Dissolve"), QStringLiteral("diss"))
      << qMakePair(QObject::tr("Darken"), QStringLiteral("dark")) << qMakePair(QObject::tr("Multiply"), QStringLiteral("mul"))
      << qMakePair(QObject::tr("Color Burn"), QStringLiteral("idiv")) << qMakePair(QObject::tr("Linear Burn"), QStringLiteral("lbrn"))
      << qMakePair(QObject::tr("Darker Color"), QStringLiteral("dkCl")) << qMakePair(QObject::tr("Lighten"), QStringLiteral("lite"))
      << qMakePair(QObject::tr("Screen"), QStringLiteral("scrn")) << qMakePair(QObject::tr("Color Dodge"), QStringLiteral("div"))
      << qMakePair(QObject::tr("Linear Dodge (Add)"), QStringLiteral("lddg"))
      << qMakePair(QObject::tr("Lighter Color"), QStringLiteral("lgCl")) << qMakePair(QObject::tr("Overlay"), QStringLiteral("over"))
      << qMakePair(QObject::tr("Soft Light"), QStringLiteral("sLit")) << qMakePair(QObject::tr("Hard Light"), QStringLiteral("hLit"))
      << qMakePair(QObject::tr("Vivid Light"), QStringLiteral("vLit")) << qMakePair(QObject::tr("Linear Light"), QStringLiteral("lLit"))
      << qMakePair(QObject::tr("Pin Light"), QStringLiteral("pLit")) << qMakePair(QObject::tr("Hard Mix"), QStringLiteral("hMix"))
      << qMakePair(QObject::tr("Difference"), QStringLiteral("diff")) << qMakePair(QObject::tr("Exclusion"), QStringLiteral("smud"))
      << qMakePair(QObject::tr("Subtract"), QStringLiteral("fsub")) << qMakePair(QObject::tr("Divide"), QStringLiteral("fdiv"))
      << qMakePair(QObject::tr("Hue"), QStringLiteral("hue")) << qMakePair(QObject::tr("Saturation"), QStringLiteral("sat"))
      << qMakePair(QObject::tr("Color"), QStringLiteral("colr")) << qMakePair(QObject::tr("Luminosity"), QStringLiteral("lum"));
    return m;
}

// ---- LayerTree -------------------------------------------------------------------------------------

LayerTree::LayerTree(QWidget* parent) : QTreeWidget(parent) {
    setObjectName(QStringLiteral("LayerTree"));
    setColumnCount(4);
    setHeaderHidden(true);
    setRootIsDecorated(true);
    setIndentation(14);
    setIconSize(QSize(kThumb, kThumb));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setUniformRowHeights(false);
    setAlternatingRowColors(false);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(kColEye, QHeaderView::Fixed);
    header()->setSectionResizeMode(kColMain, QHeaderView::Stretch);
    header()->setSectionResizeMode(kColMask, QHeaderView::Fixed);
    header()->setSectionResizeMode(kColFlags, QHeaderView::Fixed);
    header()->resizeSection(kColEye, 28);
    header()->resizeSection(kColMask, kThumb + 8);
    header()->resizeSection(kColFlags, 24);
    // The tree column is the main one so group children indent under their group's name.
    setTreePosition(kColMain);
}

void LayerTree::dropEvent(QDropEvent* e) {
    QTreeWidgetItem* dragged = currentItem();
    if (!dragged) return e->ignore();
    const QString id = dragged->data(kColMain, kIdRole).toString();
    QTreeWidgetItem* target = itemAt(e->position().toPoint());
    QString parent = QStringLiteral("root");
    int index = 0;
    const auto doc_index = [](QTreeWidgetItem* it) {
        // Items are listed top of the stack first, so doc index = count - 1 - visual row.
        QTreeWidgetItem* p = it->parent();
        const int count = p ? p->childCount() : it->treeWidget()->topLevelItemCount();
        const int row = p ? p->indexOfChild(it) : it->treeWidget()->indexOfTopLevelItem(it);
        return count - 1 - row;
    };
    const auto parent_id = [](QTreeWidgetItem* it) {
        return it->parent() ? it->parent()->data(kColMain, kIdRole).toString() : QStringLiteral("root");
    };
    if (!target) {
        parent = QStringLiteral("root");
        index = 0;  // dropped below every row: bottom of the stack
    } else {
        const bool is_group = target->data(kColMain, kIdRole + 1).toBool();
        switch (dropIndicatorPosition()) {
            case QAbstractItemView::OnItem:
                if (is_group) {
                    parent = target->data(kColMain, kIdRole).toString();
                    index = target->childCount();  // top of the group
                } else {
                    parent = parent_id(target);
                    index = doc_index(target) + 1;
                }
                break;
            case QAbstractItemView::AboveItem:
                parent = parent_id(target);
                index = doc_index(target) + 1;
                break;
            case QAbstractItemView::BelowItem:
                parent = parent_id(target);
                index = doc_index(target);
                break;
            case QAbstractItemView::OnViewport:
                parent = QStringLiteral("root");
                index = 0;
                break;
        }
        // doc 10: the index is counted after the node is removed from its old place.
        if (parent_id(dragged) == parent && doc_index(dragged) < index) --index;
    }
    e->setDropAction(Qt::IgnoreAction);
    e->accept();
    if (target == dragged) return;
    emit move_requested(id, parent, std::max(0, index));
}

// ---- LayersPanel --------------------------------------------------------------------------------------

LayersPanel::LayersPanel(EditorSession* s, QWidget* parent) : QWidget(parent), s_(s) {
    setObjectName(QStringLiteral("LayersPanel"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* head = new QWidget;
    head->setObjectName(QStringLiteral("PanelHeaderRow"));
    auto* hg = new QVBoxLayout(head);
    hg->setContentsMargins(6, 5, 6, 5);
    hg->setSpacing(4);
    auto* r1 = new QHBoxLayout;
    blend_ = new QComboBox;
    blend_->setObjectName(QStringLiteral("BlendMode"));
    blend_->setToolTip(tr("Blend mode"));
    blend_->setMaxVisibleItems(30);
    opacity_ = new QSpinBox;
    opacity_->setObjectName(QStringLiteral("Opacity"));
    opacity_->setRange(0, 100);
    opacity_->setSuffix(QStringLiteral(" %"));
    opacity_->setKeyboardTracking(false);
    opacity_->setToolTip(tr("Opacity: fades the layer including its blending"));
    auto* ol = new QLabel(tr("Opacity"));
    r1->addWidget(blend_, 1);
    r1->addWidget(ol);
    r1->addWidget(opacity_);
    auto* r2 = new QHBoxLayout;
    auto* ll = new QLabel(tr("Lock"));
    lock_ = new QToolButton;
    lock_->setObjectName(QStringLiteral("LockTransparency"));
    lock_->setIcon(icon(QStringLiteral("lock")));
    lock_->setCheckable(true);
    lock_->setAutoRaise(true);
    lock_->setToolTip(tr("Lock transparency: painting keeps the layer's alpha"));
    clip_ = new QToolButton;
    clip_->setObjectName(QStringLiteral("ClipToggle"));
    clip_->setIcon(icon(QStringLiteral("clip")));
    clip_->setCheckable(true);
    clip_->setAutoRaise(true);
    clip_->setToolTip(tr("Clip to the layer below (clipping mask)"));
    fill_ = new QSpinBox;
    fill_->setObjectName(QStringLiteral("Fill"));
    fill_->setRange(0, 100);
    fill_->setSuffix(QStringLiteral(" %"));
    fill_->setKeyboardTracking(false);
    fill_->setToolTip(tr("Fill: fades the layer's pixels but not its blending effect"));
    r2->addWidget(ll);
    r2->addWidget(lock_);
    r2->addWidget(clip_);
    r2->addStretch(1);
    r2->addWidget(new QLabel(tr("Fill")));
    r2->addWidget(fill_);
    hg->addLayout(r1);
    hg->addLayout(r2);
    for (QLabel* l : head->findChildren<QLabel*>()) l->setStyleSheet(QStringLiteral("color:#8d9199;"));
    v->addWidget(head);

    tree_ = new LayerTree;
    v->addWidget(tree_, 1);

    auto* foot = new QWidget;
    foot->setObjectName(QStringLiteral("PanelFooterRow"));
    footer_ = new QHBoxLayout(foot);
    footer_->setContentsMargins(4, 3, 4, 3);
    footer_->setSpacing(2);
    v->addWidget(foot);

    connect(s_, &EditorSession::tree_changed, this, &LayersPanel::rebuild);
    connect(s_, &EditorSession::document_reset, this, &LayersPanel::rebuild);
    connect(s_, &EditorSession::active_layer_changed, this, [this] {
        if (rebuilding_) return;
        rebuild();
    });
    connect(tree_, &QTreeWidget::itemClicked, this, &LayersPanel::on_item_clicked);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, &LayersPanel::on_item_double_clicked);
    tree_->viewport()->installEventFilter(this);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* it = tree_->itemAt(pos);
        if (!it || context_actions_.isEmpty()) return;
        s_->set_active_layer(to_std(it->data(kColMain, kIdRole).toString()));
        QMenu m(this);
        for (QAction* a : context_actions_) {
            if (a) m.addAction(a);
            else m.addSeparator();
        }
        m.exec(tree_->viewport()->mapToGlobal(pos));
    });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &LayersPanel::on_selection_changed);
    connect(tree_, &LayerTree::move_requested, this, [this](const QString& id, const QString& parent, int index) {
        s_->apply({{"op", "move_layer"}, {"id", to_std(id)}, {"parent", to_std(parent)}, {"index", index}}, tr("Move Layer"));
    });
    connect(blend_, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        if (syncing_ || !s_->active_node()) return;
        s_->apply({{"op", "set_blend"}, {"layer", s_->active_layer()}, {"mode", to_std(blend_->itemData(i).toString())}},
                  tr("Blend Mode: %1").arg(blend_->itemText(i)));
    });
    connect(opacity_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        if (syncing_ || !s_->active_node()) return;
        s_->apply({{"op", "set_opacity"}, {"layer", s_->active_layer()}, {"value", v / 100.0}}, tr("Opacity %1 %").arg(v));
    });
    connect(fill_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        if (syncing_ || !s_->active_node()) return;
        s_->apply({{"op", "set_fill"}, {"layer", s_->active_layer()}, {"value", v / 100.0}}, tr("Fill %1 %").arg(v));
    });
    connect(lock_, &QToolButton::toggled, this, [this](bool on) {
        if (syncing_ || !s_->active_node()) return;
        s_->apply({{"op", "lock_transparency"}, {"layer", s_->active_layer()}, {"value", on}},
                  on ? tr("Lock Transparency") : tr("Unlock Transparency"));
    });
    connect(clip_, &QToolButton::toggled, this, [this](bool on) {
        if (syncing_ || !s_->active_node()) return;
        s_->apply({{"op", "set_clip"}, {"layer", s_->active_layer()}, {"value", on}},
                  on ? tr("Create Clipping Mask") : tr("Release Clipping Mask"));
    });
}

void LayersPanel::add_footer_action(QAction* a) {
    auto* b = new QToolButton;
    b->setDefaultAction(a);
    b->setAutoRaise(true);
    b->setIconSize(QSize(18, 18));
    footer_->addWidget(b);
}

void LayersPanel::add_footer_menu(const QIcon& ic, const QString& tip, QMenu* menu) {
    auto* b = new QToolButton;
    b->setIcon(ic);
    b->setToolTip(tip);
    b->setMenu(menu);
    b->setPopupMode(QToolButton::InstantPopup);
    b->setAutoRaise(true);
    b->setIconSize(QSize(18, 18));
    b->setObjectName(QStringLiteral("AdjustmentMenuButton"));
    footer_->addWidget(b);
}

void LayersPanel::add_footer_stretch() { footer_->addStretch(1); }

QIcon LayersPanel::layer_thumb(const rl::Node& n) const {
    if (n.is_group()) return icon(QStringLiteral("folder"));
    if (n.is_adjustment()) return icon(QStringLiteral("adjust"));
    QPixmap pm = checker_thumb();
    const int W = n.pixels.width(), H = n.pixels.height();
    if (W <= 0 || H <= 0) return QIcon(pm);
    // Nearest samples of the layer for a 30x30 preview, letterboxed to the canvas aspect.
    const double sc = std::min(static_cast<double>(kThumb) / W, static_cast<double>(kThumb) / H);
    const int tw = std::max(1, static_cast<int>(std::lround(W * sc))), th = std::max(1, static_cast<int>(std::lround(H * sc)));
    QImage img(tw, th, QImage::Format_RGBA8888);
    for (int y = 0; y < th; ++y) {
        uchar* row = img.scanLine(y);
        const int sy = std::min(H - 1, static_cast<int>((y + 0.5) / sc));
        for (int x = 0; x < tw; ++x) {
            const int sx = std::min(W - 1, static_cast<int>((x + 0.5) / sc));
            const rl::Rgba8 p = n.pixels.get(sx, sy);
            row[x * 4 + 0] = p.r;
            row[x * 4 + 1] = p.g;
            row[x * 4 + 2] = p.b;
            row[x * 4 + 3] = p.a;
        }
    }
    QPainter p(&pm);
    p.drawImage(QPoint((kThumb - tw) / 2, (kThumb - th) / 2), img);
    p.setPen(QColor(0, 0, 0, 120));
    p.drawRect(0, 0, kThumb - 1, kThumb - 1);
    return QIcon(pm);
}

QIcon LayersPanel::mask_thumb(const rl::Node& n) const {
    if (!n.mask) return {};
    const rl::GrayImage& m = n.mask->plane;
    const int W = m.width(), H = m.height();
    QPixmap pm(kThumb, kThumb);
    pm.fill(QColor(0x40, 0x40, 0x40));
    if (W > 0 && H > 0) {
        const double sc = std::min(static_cast<double>(kThumb) / W, static_cast<double>(kThumb) / H);
        const int tw = std::max(1, static_cast<int>(std::lround(W * sc))), th = std::max(1, static_cast<int>(std::lround(H * sc)));
        QImage img(tw, th, QImage::Format_Grayscale8);
        for (int y = 0; y < th; ++y) {
            uchar* row = img.scanLine(y);
            const int sy = std::min(H - 1, static_cast<int>((y + 0.5) / sc));
            for (int x = 0; x < tw; ++x) row[x] = m.get(std::min(W - 1, static_cast<int>((x + 0.5) / sc)), sy);
        }
        QPainter p(&pm);
        p.drawImage(QPoint((kThumb - tw) / 2, (kThumb - th) / 2), img);
        if (!n.mask->enabled) {
            p.setPen(QPen(QColor(220, 60, 60), 2));
            p.drawLine(3, 3, kThumb - 4, kThumb - 4);
            p.drawLine(kThumb - 4, 3, 3, kThumb - 4);
        }
        if (s_->edit_mask() && s_->active_layer() == n.id) {
            p.setPen(QPen(theme::kAccent, 2));
            p.drawRect(1, 1, kThumb - 2, kThumb - 2);
        }
    }
    return QIcon(pm);
}

void LayersPanel::add_items(QTreeWidgetItem* parent_item, const rl::Node& container) {
    for (auto it = container.children.rbegin(); it != container.children.rend(); ++it) {
        const rl::Node& n = *it;
        auto* item = parent_item ? new QTreeWidgetItem(parent_item) : new QTreeWidgetItem(tree_);
        const QString id = to_q(n.id);
        item->setData(kColMain, kIdRole, id);
        item->setData(kColMain, kIdRole + 1, n.is_group());
        item->setIcon(kColEye, icon(n.visible ? QStringLiteral("eye") : QStringLiteral("eye_off")));
        item->setToolTip(kColEye, tr("Show / hide"));
        // doc 60 §1.1: the display name is the name, or the id while the node has none.
        QString text = layer_display_name(n);
        if (n.clip) text = QStringLiteral("↳ ") + text;
        QString sub;
        if (n.is_group()) sub = n.mode == rl::BlendMode::Pass ? tr("Pass Through") : tr("Isolated, %1").arg(mode_display(n.mode));
        else if (n.is_adjustment()) {
            const std::string t = n.adjustment ? n.adjustment->type() : std::string();
            sub = rl::adjust::is_known_type(t) ? adjustment_display(t) : tr("kept from file (not rendered)");
        }
        else if (n.mode != rl::BlendMode::Norm) sub = mode_display(n.mode);
        if (n.opacity != 1.0) sub += (sub.isEmpty() ? QString() : QStringLiteral(", ")) + QStringLiteral("%1 %").arg(std::lround(n.opacity * 100));
        item->setText(kColMain, sub.isEmpty() ? text : text + QStringLiteral("\n") + sub);
        item->setSizeHint(kColMain, QSize(-1, sub.isEmpty() ? kThumb + 6 : kThumb + 12));
        item->setIcon(kColMain, layer_thumb(n));
        item->setToolTip(kColMain,
                         n.is_adjustment() ? tr("%1\nDouble-click to edit the adjustment; F2 renames.").arg(text)
                         : n.is_raster()   ? tr("%1\nDouble-click to rename; Ctrl+click the thumbnail to select its pixels.").arg(text)
                                           : tr("%1\nDouble-click to rename.").arg(text));
        if (n.is_adjustment() && !rl::adjust::is_known_type(n.adjustment ? n.adjustment->type() : "")) {
            item->setIcon(kColFlags, icon(QStringLiteral("lock")));
            item->setToolTip(kColFlags, tr("An adjustment Rasterloom cannot render or edit; it is saved back unchanged"));
        }
        if (!n.visible) item->setForeground(kColMain, theme::kTextDim);
        if (n.mask) {
            item->setIcon(kColMask, mask_thumb(n));
            item->setToolTip(kColMask, tr("Layer mask: click to paint on the mask, Shift+click to enable/disable"));
        }
        if (n.lock_alpha) {
            item->setIcon(kColFlags, icon(QStringLiteral("lock")));
            item->setToolTip(kColFlags, tr("Transparency locked"));
        }
        Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        if (n.is_group()) f |= Qt::ItemIsDropEnabled;
        item->setFlags(f);
        if (n.is_group()) {
            add_items(item, n);
            item->setExpanded(true);
        }
        if (n.id == s_->active_layer()) item->setSelected(true);
    }
}

void LayersPanel::rebuild() {
    rebuilding_ = true;
    // Keep collapsed groups collapsed across rebuilds.
    QSet<QString> collapsed;
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if ((*it)->childCount() > 0 && !(*it)->isExpanded()) collapsed.insert((*it)->data(kColMain, kIdRole).toString());
    const int scroll = tree_->verticalScrollBar() ? tree_->verticalScrollBar()->value() : 0;
    tree_->blockSignals(true);
    tree_->clear();
    if (s_->has_document()) add_items(nullptr, s_->doc().root());
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if (collapsed.contains((*it)->data(kColMain, kIdRole).toString())) (*it)->setExpanded(false);
    tree_->blockSignals(false);
    if (tree_->verticalScrollBar()) tree_->verticalScrollBar()->setValue(scroll);
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if ((*it)->isSelected()) {
            tree_->setCurrentItem(*it, kColMain, QItemSelectionModel::NoUpdate);
            break;
        }
    rebuilding_ = false;
    sync_controls();
}

void LayersPanel::refresh_thumbnails() {
    if (!s_->has_document()) return;
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        const std::string id = to_std((*it)->data(kColMain, kIdRole).toString());
        if (const rl::Node* n = find_node(s_->doc().root(), id)) {
            (*it)->setIcon(kColMain, layer_thumb(*n));
            if (n->mask) (*it)->setIcon(kColMask, mask_thumb(*n));
        }
    }
}

void LayersPanel::sync_controls() {
    syncing_ = true;
    const rl::Node* n = s_->active_node();
    const bool has = n != nullptr;
    blend_->setEnabled(has);
    opacity_->setEnabled(has);
    fill_->setEnabled(has && !n->is_group());
    lock_->setEnabled(has && n->is_raster());
    clip_->setEnabled(has && !n->is_group());
    blend_->clear();
    const bool group = has && n->is_group();
    for (const auto& [name, json] : blend_modes(group)) blend_->addItem(name, json);
    if (!group) {
        // Visual separators between the W3C families.
        for (int at : {23, 19, 12, 7, 2}) blend_->insertSeparator(at);
    }
    if (has) {
        const int i = blend_->findData(QString::fromLatin1(rl::blend_mode_name(n->mode)));
        blend_->setCurrentIndex(i);
        opacity_->setValue(static_cast<int>(std::lround(n->opacity * 100)));
        fill_->setValue(n->is_group() ? 100 : static_cast<int>(std::lround(n->fill * 100)));
        lock_->setChecked(n->is_raster() && n->lock_alpha);
        clip_->setChecked(!n->is_group() && n->clip);
    }
    syncing_ = false;
}

void LayersPanel::on_selection_changed() {
    if (rebuilding_) return;
    const auto sel = tree_->selectedItems();
    if (sel.isEmpty()) return;
    rebuilding_ = true;
    s_->set_active_layer(to_std(sel.front()->data(kColMain, kIdRole).toString()));
    rebuilding_ = false;
    sync_controls();
    refresh_thumbnails();
}

void LayersPanel::on_item_clicked(QTreeWidgetItem* item, int column) {
    const std::string id = to_std(item->data(kColMain, kIdRole).toString());
    const rl::Node* n = s_->has_document() ? find_node(s_->doc().root(), id) : nullptr;
    if (!n) return;
    if (column == kColEye) {
        s_->apply({{"op", "set_visible"}, {"layer", id}, {"value", !n->visible}},
                  n->visible ? tr("Hide %1").arg(layer_display_name(*n)) : tr("Show %1").arg(layer_display_name(*n)));
    } else if (column == kColMask && n->mask) {
        if (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) {
            s_->apply({{"op", "set_mask_enabled"}, {"layer", id}, {"value", !n->mask->enabled}},
                      n->mask->enabled ? tr("Disable Layer Mask") : tr("Enable Layer Mask"));
        } else {
            s_->set_active_layer(id);
            s_->set_edit_mask(!s_->edit_mask());
            refresh_thumbnails();
        }
    } else if (column == kColMain) {
        if (s_->edit_mask()) {
            s_->set_edit_mask(false);
            refresh_thumbnails();
        }
    }
}

void LayersPanel::on_item_double_clicked(QTreeWidgetItem* item, int column) {
    if (column != kColMain) return;
    const std::string id = to_std(item->data(kColMain, kIdRole).toString());
    const rl::Node* n = s_->has_document() ? find_node(s_->doc().root(), id) : nullptr;
    if (!n) return;
    if (n->is_adjustment()) {
        emit edit_adjustment_requested(id);
        return;
    }
    begin_rename(id);
}

QTreeWidgetItem* LayersPanel::item_for(const std::string& id) const {
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if (to_std((*it)->data(kColMain, kIdRole).toString()) == id) return *it;
    return nullptr;
}

void LayersPanel::begin_rename(const std::string& id) {
    QTreeWidgetItem* item = item_for(id);
    const rl::Node* n = s_->has_document() ? find_node(s_->doc().root(), id) : nullptr;
    if (!item || !n) return;
    end_rename(false);
    tree_->scrollToItem(item);
    QRect r = tree_->visualItemRect(item);
    const QRect col = tree_->visualRect(tree_->indexFromItem(item, kColMain));
    r.setLeft(col.left() + kThumb + 10);
    r.setRight(col.right() - 2);
    r.setHeight(std::min(r.height(), 26));
    rename_ = new QLineEdit(tree_->viewport());
    rename_->setObjectName(QStringLiteral("LayerRename"));
    rename_->setText(layer_display_name(*n));
    rename_->setMaxLength(255);
    rename_->setGeometry(r.adjusted(0, 3, 0, 3));
    rename_->selectAll();
    rename_->show();
    rename_->setFocus(Qt::OtherFocusReason);
    rename_id_ = id;
    rename_->installEventFilter(this);
    connect(rename_, &QLineEdit::returnPressed, this, [this] { end_rename(true); });
    connect(rename_, &QLineEdit::editingFinished, this, [this] {
        if (rename_ && !rename_->hasFocus()) end_rename(true);  // clicking elsewhere commits, like other editors
    });
}

void LayersPanel::end_rename(bool commit) {
    if (!rename_) return;
    QLineEdit* ed = rename_;
    rename_ = nullptr;
    const std::string id = rename_id_;
    const QString text = ed->text();
    ed->hide();
    ed->deleteLater();
    if (commit) rename(id, text);
}

bool LayersPanel::rename(const std::string& id, const QString& raw) {
    const rl::Node* n = s_->has_document() ? find_node(s_->doc().root(), id) : nullptr;
    if (!n) return false;
    // doc 60 §1.1: no control characters, at most 255 code points.
    QString name;
    for (const QChar c : raw)
        if (c.unicode() >= 0x20 && c.unicode() != 0x7f) name += c;
    name = name.trimmed();
    const QList<uint> cps = name.toUcs4();
    if (cps.size() > 255) name = QString::fromUcs4(reinterpret_cast<const char32_t*>(cps.constData()), 255);
    const QString current = layer_display_name(*n);
    if (name == current) return false;
    // doc 60 §3: set_name changes only the display name (ids are immutable, §1.2). Typing the id
    // itself, or clearing the field, resets the name so the row shows the id again.
    return s_->apply({{"op", "set_name"}, {"layer", id}, {"name", name == to_q(n->id) ? std::string() : to_std(name)}},
                     tr("Rename to %1").arg(name.isEmpty() ? to_q(n->id) : name))
        .ok();
}

bool LayersPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == rename_ && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
        end_rename(false);
        return true;
    }
    if (o == tree_->viewport() && e->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(e);
        press_pos_ = me->position().toPoint();
        press_mods_ = me->modifiers();
        // Ctrl+click on a raster layer's thumbnail: selection from its alpha (doc 60 §7).
        if (me->button() == Qt::LeftButton && (me->modifiers() & Qt::ControlModifier)) {
            QTreeWidgetItem* it = tree_->itemAt(press_pos_);
            if (it) {
                const QRect col = tree_->visualRect(tree_->indexFromItem(it, kColMain));
                if (press_pos_.x() >= col.left() && press_pos_.x() <= col.left() + kThumb + 8) {
                    const std::string id = to_std(it->data(kColMain, kIdRole).toString());
                    const rl::Node* n = s_->has_document() ? find_node(s_->doc().root(), id) : nullptr;
                    if (n && n->is_raster()) {
                        emit select_alpha_requested(id, me->modifiers());
                        return true;
                    }
                }
            }
        }
    }
    return QWidget::eventFilter(o, e);
}

}  // namespace rl::gui
