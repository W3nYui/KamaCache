#pragma once

#include "KArcCacheNode.h"
#include <unordered_map>
#include <map>
#include <mutex>

namespace KamaCache 
{

template<typename Key, typename Value>
/**
 * @brief 构建具有幽灵缓存表的LFU算法
 * 
 */
class ArcLfuPart 
{
public:
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>; // 构建指针
    using NodeMap = std::unordered_map<Key, NodePtr>; // 用于O(1)查找的LFU指针表
    using FreqMap = std::map<size_t, std::list<NodePtr>>; // 频率表与双向链表
    /**
     * @brief 构造函数
     * 
     * @param capacity 缓存容量
     * @param transformThreshold 从LRU晋升到LFU的阈值 在LFU中没有被采用
     */
    explicit ArcLfuPart(size_t capacity, size_t transformThreshold)
        : capacity_(capacity)
        , ghostCapacity_(capacity)
        , transformThreshold_(transformThreshold)
        , minFreq_(0)
    {
        initializeLists();
    }
    /**
     * @brief 将数据写入LFU缓存函数
     * 
     * @param key 
     * @param value 
     * @return true 
     * @return false 
     */
    bool put(Key key, Value value) 
    {
        if (capacity_ == 0) 
            return false;
        // 锁住整个LFU
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end())     // 如果命中缓存 则更新
        {
            return updateExistingNode(it->second, value);
        }
        return addNewNode(key, value);  // 未命中缓存 则插入
    }

    /**
     * @brief 得到数据值，并返回是否存在
     * 
     * @param key 
     * @param value 
     * @return true 
     * @return false 
     */
    bool get(Key key, Value& value) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        // 映射表中存在 就更新 提高访问频次
        if (it != mainCache_.end()) 
        {
            updateNodeFrequency(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    // 查找LFU的缓存
    bool contain(Key key)
    {
        return mainCache_.find(key) != mainCache_.end();
    }

    /**
     * @brief 幽灵缓存命中 将其从幽灵缓存中删除
     * 
     * @param key 
     * @return true 返回幽灵缓存是否命中
     * @return false 
     */
    bool checkGhost(Key key) 
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) 
        {
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

    // 提升容量
    void increaseCapacity() { ++capacity_; }
    
    /**
     * @brief 减小LFU的容量，如果当前容量已满，则需要先清除LFU缓存
     * 
     * @return true 修改成功
     * @return false 如果容量已经最小 则不可修改
     */
    bool decreaseCapacity() 
    {
        if (capacity_ <= 0) return false;
        if (mainCache_.size() == capacity_) 
        {
            evictLeastFrequent();
        }
        --capacity_;
        return true;
    }

private:
    /**
     * @brief 初始化一个LFU缓存 包括了幽灵表的头尾哨兵节点
     * 
     */
    void initializeLists() 
    {
        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    /**
     * @brief 更新被命中缓存的数值 同时提高访问频次等级
     * 
     * @param node 深拷贝了一个 shared_ptr，根据指针语义可以对原值进行修改 可以改进为浅拷贝：const NodePtr & node
     * @param value 
     * @return true 
     * @return false 
     */
    bool updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        updateNodeFrequency(node);
        return true;
    }

    /**
     * @brief 插入一个新的结点
     * 
     * @param key 
     * @param value 
     * @return true 
     * @return false 
     */
    bool addNewNode(const Key& key, const Value& value) 
    {
        // LFU容量超出 则需要清理缓存
        if (mainCache_.size() >= capacity_) 
        {
            evictLeastFrequent();
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value);
        mainCache_[key] = newNode;
        
        // 将新节点添加到频率为1的列表中
        if (freqMap_.find(1) == freqMap_.end()) 
        {
            freqMap_[1] = std::list<NodePtr>();
        }
        freqMap_[1].push_back(newNode);
        minFreq_ = 1;
        
        return true;
    }

    /**
     * @brief 更新当前节点的频次等级
     * 
     * @param node 深拷贝了一个 shared_ptr
     */
    void updateNodeFrequency(NodePtr node) 
    {
        size_t oldFreq = node->getAccessCount();
        node->incrementAccessCount(); // 提高当前节点的自身属性：访问频次
        size_t newFreq = node->getAccessCount();

        // 从旧频率列表中移除节点
        auto& oldList = freqMap_[oldFreq];
        oldList.remove(node);
        // 维护频次表与最小访问频率
        if (oldList.empty()) 
        {
            freqMap_.erase(oldFreq);
            if (oldFreq == minFreq_) 
            {
                minFreq_ = newFreq;
            }
        }

        // 添加到新频率列表
        // 如果频次表没有新频率，需要构造新频次表与对应链表 key->list<NodePtr>
        if (freqMap_.find(newFreq) == freqMap_.end()) 
        {
            freqMap_[newFreq] = std::list<NodePtr>();
        }
        // 插入新节点 新节点在链表后面，旧节点在链表前
        freqMap_[newFreq].push_back(node);
    }

    /**
     * @brief 清除LFU缓存 清除最小频率链表的最旧未使用数据
     * 
     */
    void evictLeastFrequent() 
    {
        if (freqMap_.empty()) 
            return;

        // 获取最小频率的列表
        auto& minFreqList = freqMap_[minFreq_];
        // 如果最小频率不存在，那么会创建一个最小频率链表，则需要判空
        // 避免了minFreq_维护错误导致的崩溃
        if (minFreqList.empty()) 
            return;

        // 移除最少使用的节点
        NodePtr leastNode = minFreqList.front();
        minFreqList.pop_front();

        // 如果该频率的列表为空，则删除该频率项
        if (minFreqList.empty()) 
        {
            freqMap_.erase(minFreq_);
            // 更新最小频率
            if (!freqMap_.empty()) 
            {
                minFreq_ = freqMap_.begin()->first;
            }
        }

        // 幽灵缓存过大，则需要清除
        if (ghostCache_.size() >= ghostCapacity_) 
        {
            // 幽灵缓存表的清除
            removeOldestGhost();
        }
        // 将节点移到幽灵缓存
        addToGhost(leastNode);
        
        // 从主缓存中移除键值对
        mainCache_.erase(leastNode->getKey());
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

    /**
     * @brief 将节点插入幽灵缓存的尾部 并更新映射表
     * 
     * @param node 
     */
    void addToGhost(NodePtr node) 
    {
        node->next_ = ghostTail_;
        node->prev_ = ghostTail_->prev_;
        if (!ghostTail_->prev_.expired()) {
            ghostTail_->prev_.lock()->next_ = node;
        }
        ghostTail_->prev_ = node;
        ghostCache_[node->getKey()] = node;
    }

    void removeOldestGhost() 
    {
        NodePtr oldestGhost = ghostHead_->next_;
        // 避免错误调用 导致哨兵节点被删除
        if (oldestGhost != ghostTail_) 
        {
            removeFromGhost(oldestGhost);
            // 从幽灵缓存中清除键值对
            ghostCache_.erase(oldestGhost->getKey());
        }
    }

private:
    size_t capacity_; // 缓存大小
    size_t ghostCapacity_; // 幽灵缓存表大小
    size_t transformThreshold_; // LRU->LFU的晋升阈值
    size_t minFreq_; // 当前最小访问频次
    std::mutex mutex_; // 互斥锁

    NodeMap mainCache_;  // 主LFU缓存表
    NodeMap ghostCache_; // 幽灵LFU缓存表
    FreqMap freqMap_;    // LFU根据访问频次分组的双向链表
    
    NodePtr ghostHead_;  // 幽灵表的哨兵头
    NodePtr ghostTail_;  // 幽灵表的哨兵尾
};

} // namespace KamaCache