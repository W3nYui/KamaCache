#pragma once

#include "KArcCacheNode.h"
#include <unordered_map>
#include <mutex>

namespace KamaCache 
{

template<typename Key, typename Value>
class ArcLruPart 
{
public:
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    /**
     * @brief 构造函数
     * 
     * @param capacity 容量
     * @param transformThreshold LRU -> LFU 阈值 
     */
    explicit ArcLruPart(size_t capacity, size_t transformThreshold)
        : capacity_(capacity)
        , ghostCapacity_(capacity)
        , transformThreshold_(transformThreshold)
    {
        initializeLists();
    }

    /**
     * @brief 写入/更新数据
     * 
     * @param key 
     * @param value 
     * @return true 
     * @return false 
     */
    bool put(Key key, Value value) 
    {
        if (capacity_ == 0) return false;
        // 查找缓存表 更新数据/写入数据
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            return updateExistingNode(it->second, value);
        }
        return addNewNode(key, value);
    }

    /**
     * @brief 访问节点 需要提升该节点的访问次数
     * 
     * @param key 
     * @param value 浅拷贝 用于返回数值
     * @param shouldTransform 浅拷贝 用于ARC判断是否加入LFU中
     * @return true 返回布尔型 用于判断是否找到该节点
     * @return false 
     */
    bool get(Key key, Value& value, bool& shouldTransform) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            shouldTransform = updateNodeAccess(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    /**
     * @brief 幽灵缓存命中 将其从幽灵缓存中删除
     * 
     * @param key 
     * @return true 
     * @return false 
     */
    bool checkGhost(Key key) 
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) {
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

    /**
     * @brief 提高LRU缓存容量
     * 
     */
    void increaseCapacity() { ++capacity_; }
    
    /**
     * @brief 降低缓存容量 需要判断能否降低：最小容量为1 如果容量过小 需要清除LRU缓存再减小
     * 
     * @return true 
     * @return false 
     */
    bool decreaseCapacity() 
    {
        if (capacity_ <= 0) return false;
        if (mainCache_.size() == capacity_) {
            evictLeastRecent();
        }
        --capacity_;
        return true;
    }

private:
    /**
     * @brief 初始化函数 构造缓存表与幽灵表的哨兵节点
     * 
     */
    void initializeLists() 
    {
        mainHead_ = std::make_shared<NodeType>();
        mainTail_ = std::make_shared<NodeType>();
        mainHead_->next_ = mainTail_;
        mainTail_->prev_ = mainHead_;

        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    bool updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        moveToFront(node); // LRU性质 头为最近使用 尾部为最长未使用 移动该节点到头节点：删除后重新添加即可
        return true;
    }

    bool addNewNode(const Key& key, const Value& value) 
    {
        if (mainCache_.size() >= capacity_) 
        {   
            evictLeastRecent(); // 驱逐最近最少访问
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value);
        mainCache_[key] = newNode;
        addToFront(newNode);
        return true;
    }

    /**
     * @brief 提升该节点等级 迁移到链表头，并提高节点被访问次数
     * 
     * @param node 
     * @return true 返回该节点是否超过阈值 需要迁移进LFU中
     * @return false 
     */
    bool updateNodeAccess(NodePtr node) 
    {
        moveToFront(node);
        node->incrementAccessCount();
        return node->getAccessCount() >= transformThreshold_;
    }

    void moveToFront(NodePtr node) 
    {
        // 先从当前位置移除
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // 清空指针，防止悬垂引用
        }
        
        // 添加到头部
        addToFront(node);
    }

    /**
     * @brief 将节点移动到最近访问端 即链表头
     * 
     * @param node 
     */
    void addToFront(NodePtr node) 
    {
        node->next_ = mainHead_->next_;
        node->prev_ = mainHead_;
        mainHead_->next_->prev_ = node;
        mainHead_->next_ = node;
    }
    
    /**
     * @brief  驱逐最近最少访问 减少LRU缓存
     * 这里设定LRU缓存链表表示： 最近使用节点(头) -> 最久为使用节点(尾) 
     * 
     */
    void evictLeastRecent() 
    {
        NodePtr leastRecent = mainTail_->prev_.lock();
        if (!leastRecent || leastRecent == mainHead_) 
            return;

        // 从主链表中移除
        removeFromMain(leastRecent);

        // 添加到幽灵缓存
        if (ghostCache_.size() >= ghostCapacity_) 
        {
            removeOldestGhost();
        }
        addToGhost(leastRecent);

        // 从主缓存映射中移除
        mainCache_.erase(leastRecent->getKey());
    }

    void removeFromMain(NodePtr node) 
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // 清空指针，防止悬垂引用
        }
    }

    void removeFromGhost(NodePtr node) 
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // 清空指针，防止悬垂引用
        }
    }

    void addToGhost(NodePtr node) 
    {
        // 重置节点的访问计数
        node->accessCount_ = 1;
        
        // 添加到幽灵缓存的头部
        node->next_ = ghostHead_->next_;
        node->prev_ = ghostHead_;
        ghostHead_->next_->prev_ = node;
        ghostHead_->next_ = node;
        
        // 添加到幽灵缓存映射
        ghostCache_[node->getKey()] = node;
    }

    void removeOldestGhost() 
    {
        // 使用lock()方法，并添加null检查
        NodePtr oldestGhost = ghostTail_->prev_.lock();
        if (!oldestGhost || oldestGhost == ghostHead_) 
            return;

        removeFromGhost(oldestGhost);
        ghostCache_.erase(oldestGhost->getKey());
    }
    

private:
    size_t capacity_;   // LRU 缓存容量
    size_t ghostCapacity_; // LRU 幽灵缓存容量
    size_t transformThreshold_; // LRU -> LFU 的转换门槛值
    std::mutex mutex_;

    NodeMap mainCache_; // LRU缓存表 key -> 节点指针
    NodeMap ghostCache_; // LRU 幽灵缓存表
    
    // 主链表
    NodePtr mainHead_; // 主缓存的哨兵节点头
    NodePtr mainTail_; // 主缓存的哨兵节点尾
    // 淘汰链表
    NodePtr ghostHead_; // 幽灵缓存的哨兵节点头
    NodePtr ghostTail_; // 幽灵缓存的哨兵节点尾
};

} // namespace KamaCache