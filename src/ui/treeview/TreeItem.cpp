#include "TreeItem.h"

TreeItem::TreeItem(const ConnectionFolder &folder, TreeItem *parent)
    : m_nodeType(TreeNodeType::Folder), m_folder(folder), m_parent(parent)
{
}

TreeItem::TreeItem(const ConnectionEntry &entry, TreeItem *parent)
    : m_nodeType(TreeNodeType::Connection), m_connection(entry), m_parent(parent)
{
}

TreeItem::~TreeItem()
{
    qDeleteAll(m_children);
}

TreeItem *TreeItem::child(int row) const
{
    if (row < 0 || row >= m_children.size())
        return nullptr;
    return m_children.at(row);
}

int TreeItem::childCount() const
{
    return m_children.size();
}

int TreeItem::row() const
{
    if (m_parent) {
        return m_parent->m_children.indexOf(const_cast<TreeItem *>(this));
    }
    return 0;
}

TreeItem *TreeItem::parentItem() const
{
    return m_parent;
}

void TreeItem::appendChild(TreeItem *child)
{
    child->m_parent = this;
    m_children.append(child);
}

void TreeItem::insertChild(int row, TreeItem *child)
{
    child->m_parent = this;
    m_children.insert(row, child);
}

void TreeItem::removeChild(int row)
{
    if (row >= 0 && row < m_children.size()) {
        delete m_children.takeAt(row);
    }
}

TreeItem *TreeItem::takeChild(int row)
{
    if (row >= 0 && row < m_children.size()) {
        TreeItem *child = m_children.takeAt(row);
        child->m_parent = nullptr;
        return child;
    }
    return nullptr;
}

void TreeItem::removeAllChildren()
{
    qDeleteAll(m_children);
    m_children.clear();
}

QString TreeItem::name() const
{
    return m_nodeType == TreeNodeType::Folder ? m_folder.name : m_connection.name;
}

qint64 TreeItem::itemId() const
{
    return m_nodeType == TreeNodeType::Folder ? m_folder.id : m_connection.id;
}
