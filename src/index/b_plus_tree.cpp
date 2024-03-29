/**
 * b_plus_tree.cpp
 */
#include <iostream>
#include <string>

#include "common/exception.h"
#include "common/logger.h"
#include "common/rid.h"
#include "index/b_plus_tree.h"
#include "page/header_page.h"

namespace cmudb {

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(const std::string &name,
                                BufferPoolManager *buffer_pool_manager,
                                const KeyComparator &comparator,
                                page_id_t root_page_id)
    : index_name_(name), root_page_id_(root_page_id),
      buffer_pool_manager_(buffer_pool_manager), comparator_(comparator) {}

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::IsEmpty() const { 
  return root_page_id_ == INVALID_PAGE_ID;
}
/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::GetValue(const KeyType &key,
                              std::vector<ValueType> &result,
                              Transaction *transaction) {
  if (root_page_id_ == INVALID_PAGE_ID) {
    return false;
  }
  mtx.lock();
  auto leaf_page = FindLeafPage(key, false);
  ValueType tmp_value;
  if (leaf_page->Lookup(key, tmp_value, comparator_)) {
    result.push_back(tmp_value);
  }
  buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), true);
  mtx.unlock();
  return true;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value,
                            Transaction *transaction) {
  mtx.lock();
  if (IsEmpty()) {
    StartNewTree(key, value);
    mtx.unlock();
    return true;
  }
  auto ret = InsertIntoLeaf(key, value, transaction);
  mtx.unlock();
  return ret;
}
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::StartNewTree(const KeyType &key, const ValueType &value) {
  auto root_page = buffer_pool_manager_->NewPage(root_page_id_);
  if (root_page == nullptr) {
    throw "out of memory";
  }
  UpdateRootPageId(true);
  auto node = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>*>(root_page);
  node->Init(root_page_id_);
  node->Insert(key, value, comparator_);
  buffer_pool_manager_->UnpinPage(root_page_id_, true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immdiately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::InsertIntoLeaf(const KeyType &key, const ValueType &value,
                                    Transaction *transaction) {
  auto leaf_page = FindLeafPage(key, false);
  ValueType tmp_value;
  if (leaf_page->Lookup(key, tmp_value, comparator_)) {
    return false;
  }
  if (leaf_page->GetSize() < leaf_page->GetMaxSize()) {
    leaf_page->Insert(key, value, comparator_);
    buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), true);
  } else {
    auto new_page = Split(leaf_page);
    if (comparator_(key, new_page->KeyAt(0)) < 0) {
      leaf_page->Insert(key, value, comparator_);
    } else {
      new_page->Insert(key, value, comparator_);
    }
    new_page->SetNextPageId(leaf_page->GetNextPageId());
    leaf_page->SetNextPageId(new_page->GetPageId());
    InsertIntoParent(leaf_page, new_page->KeyAt(0), new_page);
    buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(new_page->GetPageId(), true);
  }
  return true;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N> N *BPLUSTREE_TYPE::Split(N *node) {
  page_id_t new_page_id;
  auto new_page = buffer_pool_manager_->NewPage(new_page_id);
  if (new_page == nullptr) {
    throw "out of memory";
  }
  auto new_node = reinterpret_cast<N*>(new_page);
  new_node->Init(new_page_id, node->GetParentPageId());

  node->MoveHalfTo(new_node, buffer_pool_manager_);
  return new_node;
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertIntoParent(BPlusTreePage *old_node,
                                      const KeyType &key,
                                      BPlusTreePage *new_node,
                                      Transaction *transaction) {
  if (old_node->IsRootPage()) {
    auto root_page = buffer_pool_manager_->NewPage(root_page_id_);
    if (root_page == nullptr) {
      throw "out of memory";
    }
    UpdateRootPageId(false);
    auto node = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator>*>(root_page);
    node->Init(root_page_id_);
    node->SetValueAt(0, old_node->GetPageId());
    node->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
    old_node->SetParentPageId(root_page_id_);
    new_node->SetParentPageId(root_page_id_);
    buffer_pool_manager_->UnpinPage(root_page_id_, true);
  } else {
    page_id_t parent_page_id = old_node->GetParentPageId();
    auto parent_page = buffer_pool_manager_->FetchPage(parent_page_id);
    auto parent_node = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator>*>(parent_page);
    if (parent_node->GetSize() < parent_node->GetMaxSize()) {
      parent_node->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
    } else {
      //父节点仍要分裂
      auto new_parent_node = Split(parent_node);
      if (comparator_(key, new_parent_node->KeyAt(0)) < 0) {
        parent_node->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
      } else {
        new_parent_node->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
        new_node->SetParentPageId(new_parent_node->GetPageId());
      }
      InsertIntoParent(parent_node, new_parent_node->KeyAt(0), new_parent_node, transaction);
      buffer_pool_manager_->UnpinPage(new_parent_node->GetPageId(), true);
    }
    buffer_pool_manager_->UnpinPage(parent_page_id, true);
  }
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immdiately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key, Transaction *transaction) {
  if (IsEmpty()) {
    return;
  }
  mtx.lock();
  auto leaf_page = FindLeafPage(key, false);
  ValueType tmp_value;
  if (!leaf_page->Lookup(key, tmp_value, comparator_)) {
    buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), true);
    mtx.unlock();
    //std::cout << "Unfind" << std::endl;
    return;
  }
  //std::cout << "*****************************************************" << leaf_page->GetNextPageId() << std::endl;
  leaf_page->RemoveAndDeleteRecord(key, comparator_);
  CoalesceOrRedistribute(leaf_page, transaction);
  buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), true);
  mtx.unlock();
}

/*
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
bool BPLUSTREE_TYPE::CoalesceOrRedistribute(N *node, Transaction *transaction) {
  if (node->GetSize() >= (node->GetMaxSize() + 1) / 2) {
    return true;
  }
  if (node->IsRootPage()) {
    if (node->IsLeafPage() && node->GetSize() == 0) {
      root_page_id_ = INVALID_PAGE_ID;
      UpdateRootPageId(false);
      buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
      buffer_pool_manager_->DeletePage(node->GetPageId());
      return true;
    } else if (!node->IsLeafPage() && node->GetSize() == 1) {
      return AdjustRoot(node);
    } else {
      return true;
    }
  }
  page_id_t parent_page_id = node->GetParentPageId();
  auto parent_page = buffer_pool_manager_->FetchPage(parent_page_id);
  auto parent_node = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator>*>(parent_page);
  auto neighbor_index_in_parent = parent_node->ValueIndex(node->GetPageId()) - 1;
  bool use_right_neighbor = 0;
  if (neighbor_index_in_parent < 0) {
    use_right_neighbor = true;
    neighbor_index_in_parent = parent_node->ValueIndex(node->GetPageId()) + 1;
  }
  page_id_t neighbor_page_id = parent_node->ValueAt(neighbor_index_in_parent);
  auto neighbor_page = buffer_pool_manager_->FetchPage(neighbor_page_id);
  auto neighbor_node = reinterpret_cast<N*>(neighbor_page);

  if (node->GetSize() + neighbor_node->GetSize() <= neighbor_node->GetMaxSize()) {
    if (use_right_neighbor) {
      Coalesce(node, neighbor_node, parent_node, 0, transaction);
      buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
    } else {
      Coalesce(neighbor_node, node, parent_node, 0, transaction);
      buffer_pool_manager_->UnpinPage(neighbor_page_id, true);
    }
    bool ret = CoalesceOrRedistribute(parent_node, transaction);
    buffer_pool_manager_->UnpinPage(parent_page_id, true);
    return ret;
  } else {
    if (use_right_neighbor) {
      Redistribute(neighbor_node, node, 0);
    } else {
      auto value_index = parent_node->ValueIndex(node->GetPageId());
      Redistribute(neighbor_node, node, parent_node->ValueAt(value_index));
    }
  }
  
  buffer_pool_manager_->UnpinPage(parent_page_id, true);
  buffer_pool_manager_->UnpinPage(neighbor_page_id, true);
  return true;
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion
 * happend
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
bool BPLUSTREE_TYPE::Coalesce(
    N *&neighbor_node, N *&node,
    BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *&parent,
    int index, Transaction *transaction) {

  node->MoveAllTo(neighbor_node, 0, buffer_pool_manager_);
  parent->Remove(parent->ValueIndex(node->GetPageId()));
  if (node->IsLeafPage()) {
    neighbor_node->SetNextPageId(node->GetNextPageId());
  }
  buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
  buffer_pool_manager_->DeletePage(node->GetPageId());
  return true;
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
void BPLUSTREE_TYPE::Redistribute(N *neighbor_node, N *node, int index) {
  if (index > 0) {
    neighbor_node->MoveLastToFrontOf(node, index, buffer_pool_manager_);
  } else if (index == 0) {
    neighbor_node->MoveFirstToEndOf(node, buffer_pool_manager_); 
  }
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happend
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::AdjustRoot(BPlusTreePage *old_root_node) {
  auto tmp_node = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator>*>(old_root_node);
  root_page_id_ = tmp_node->ValueAt(0);
  UpdateRootPageId(false);
  buffer_pool_manager_->UnpinPage(old_root_node->GetPageId(), true);
  buffer_pool_manager_->DeletePage(old_root_node->GetPageId());

  auto new_root_page = buffer_pool_manager_->FetchPage(root_page_id_);
  auto new_root_node = reinterpret_cast<BPlusTreePage*>(new_root_page);
  new_root_node->SetParentPageId(INVALID_PAGE_ID); 
  return true;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the leaftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE BPLUSTREE_TYPE::Begin() {
  auto leaf_page = FindLeafPage(KeyType(), true);
  if (leaf_page == nullptr) {
    return INDEXITERATOR_TYPE();
  }
  return INDEXITERATOR_TYPE(leaf_page, 0, buffer_pool_manager_);
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE BPLUSTREE_TYPE::Begin(const KeyType &key) {
  auto leaf_page = FindLeafPage(key, true);
  if (leaf_page == nullptr) {
    return INDEXITERATOR_TYPE();
  }
  int index = leaf_page->KeyIndex(key, comparator_);
  return INDEXITERATOR_TYPE(leaf_page, index, buffer_pool_manager_);
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 */
INDEX_TEMPLATE_ARGUMENTS
B_PLUS_TREE_LEAF_PAGE_TYPE *BPLUSTREE_TYPE::FindLeafPage(const KeyType &key,
                                                         bool leftMost) {
  auto page = buffer_pool_manager_->FetchPage(root_page_id_);
  while (true) {
    auto tmp_node = reinterpret_cast<BPlusTreePage*>(page->GetData());
    if (!tmp_node->IsLeafPage()) {
      auto node = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator>*>(page);
      page_id_t next_page_id;
      if (leftMost) {
        next_page_id = node->ValueAt(0);
      } else {
        next_page_id = node->Lookup(key, comparator_);
      }
      buffer_pool_manager_->UnpinPage(page->GetPageId(), true);
      page = buffer_pool_manager_->FetchPage(next_page_id);
    } else if (tmp_node->IsLeafPage()){
      auto node = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>*>(page);
      ValueType value;
      return node;
    }
  }
}

/*
 * Update/Insert root page id in header page(where page_id = 0, header_page is
 * defined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      defualt value is false. When set to true,
 * insert a record <index_name, root_page_id> into header page instead of
 * updating it.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::UpdateRootPageId(int insert_record) {
  HeaderPage *header_page = static_cast<HeaderPage *>(
      buffer_pool_manager_->FetchPage(HEADER_PAGE_ID));
  if (insert_record)
    // create a new record<index_name + root_page_id> in header_page
    header_page->InsertRecord(index_name_, root_page_id_);
  else
    // update root_page_id in header_page
    header_page->UpdateRecord(index_name_, root_page_id_);
  buffer_pool_manager_->UnpinPage(HEADER_PAGE_ID, true);
}

/*
 * This method is used for debug only
 * print out whole b+tree sturcture, rank by rank
 */
INDEX_TEMPLATE_ARGUMENTS
std::string BPLUSTREE_TYPE::ToString(bool verbose) { return "Empty tree"; }

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string &file_name,
                                    Transaction *transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;

    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, transaction);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string &file_name,
                                    Transaction *transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, transaction);
  }
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;
template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;
template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;
template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;
template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

} // namespace cmudb
