#pragma once 

#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "KICachePolicy.h"

namespace KamaCache
{

// 前向声明 由于KLruCache在LruNode后才定义，而LruNode中需要使用KLruCache作为友元类，因此前向定义
template<typename Key, typename Value> class KLruCache;

/**
 * @brief LRU 缓存的双向链表节点类
 * * 核心设计：
 * 1. 采用模板编程，支持任意类型的 Key 和 Value。
 * 2. 使用智能指针管理链表关系，自动处理内存释放 (RAII)。
 * 3. 记录 accessCount，为进阶的缓存淘汰算法 (如 LRU-K) 预留接口。
 */
template<typename Key, typename Value>
class LruNode 
{
private:
    Key key_;             // 存储键，用于反向在 Hash 表中查找并删除
    Value value_;         // 存储实际数据
    size_t accessCount_;  // 访问次数
    /**
     * @brief 前向指针 (Previous Pointer)
     * @note 关键点：使用 std::weak_ptr 防止循环引用 (Cyclic Reference)。
     * 如果这里用 shared_ptr，两个节点互相引用，引用计数永远不为0，导致内存泄漏。
     */
    std::weak_ptr<LruNode<Key, Value>> prev_;  // 改为weak_ptr打破循环引用
    /**
     * @brief 后向指针 (Next Pointer)
     * @note 使用 std::shared_ptr 表达"强所有权"。
     * 当前节点"拥有"下一个节点的生命周期的一部分。
     */
    std::shared_ptr<LruNode<Key, Value>> next_;

public:
    /// 构造函数：初始化键值对，默认引用计数为 1
    LruNode(Key key, Value value)
        : key_(key)
        , value_(value)
        , accessCount_(1) 
    {}

    // 提供必要的访问器
    // 在访问器的设计，外层加 const，保证不修改成员变量
    Key getKey() const { return key_; }
    Value getValue() const { return value_; }
    // Set 方法用于更新缓存值
    void setValue(const Value& value) { value_ = value; }
    size_t getAccessCount() const { return accessCount_; }
    void incrementAccessCount() { ++accessCount_; }
    // 友元声明，允许 KLruCache 访问私有成员
    friend class KLruCache<Key, Value>;
};


template<typename Key, typename Value>
class KLruCache : public KICachePolicy<Key, Value>
{
public:
    using LruNodeType = LruNode<Key, Value>;
    using NodePtr = std::shared_ptr<LruNodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    // 初始化构造函数 输入缓存容量 定义首尾哨兵节点
    KLruCache(int capacity)
        : capacity_(capacity)
    {
        initializeList();
    }
    // 虚析构函数，确保通过基类指针删除子类对象时，子类析构函数被调用
    // 默认析构函数，智能指针会自动释放内存
    ~KLruCache() override = default;

    // 子类动态多态，对基类的纯虚函数接口重写
    // 写入操作
    void put(Key key, Value value) override
    {
        // 检查容量是否有效
        if (capacity_ <= 0)
            return;
        // KRU缓存互斥锁
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        // 两种更新方式：更新已有节点，添加新节点
        if (it != nodeMap_.end())
        {
            // 如果在当前容器中,则更新value,并调用get方法，代表该数据刚被访问
            updateExistingNode(it->second, value);
            return ;
        }
        // 如果不存在map(缓存)中，则添加新节点
        addNewNode(key, value);
    }
    // 读取操作，value为传出参数 返回bool表示是否找到
    bool get(Key key, Value& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            moveToMostRecent(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    // 读取操作，返回value值
    // 复用上面的get接口
    Value get(Key key) override
    {
        Value value{};
        // memset(&value, 0, sizeof(value));   // memset 是按字节设置内存的，对于复杂类型（如 string）使用 memset 可能会破坏对象的内部结构
        get(key, value);
        return value;
    }

    // 删除指定元素
    void remove(Key key) 
    {   
        std::lock_guard<std::mutex> lock(mutex_);
        // 如果找到该key，则移除对应节点
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // 从链表中移除节点
            // 因为removeNode在moveToMostRecent中也复用
            // 因此将哈希表中的删除和链表节点的删除分开
            // 仅在完全删除节点时调用
            removeNode(it->second);
            nodeMap_.erase(it);
        }
    }

// 私有成员函数是将外部接口细化为更小的功能块，所有的复杂逻辑到最后就是私有函数内的增删改查
private:
    void initializeList()
    {
        // 创建首尾虚拟节点
        dummyHead_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyTail_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyHead_->next_ = dummyTail_;
        dummyTail_->prev_ = dummyHead_;
    }

    // 当实行写入操作时，如果该key已存在，则更新其value值
    // 同时将其移动到链表尾部，表示最近访问过
    // 这说明了写入操作也会影响缓存的访问顺序
    void updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        moveToMostRecent(node); // 更新访问顺序
    }

    // 添加新节点到缓存
    // 执行顺序：节点容量检查 -> 驱逐最少使用节点（如有必要） -> 创建新节点 -> 插入节点 -> 更新哈希表
    void addNewNode(const Key& key, const Value& value) 
    {
       if (nodeMap_.size() >= capacity_) 
       {
           evictLeastRecent();
       }

       NodePtr newNode = std::make_shared<LruNodeType>(key, value);
       insertNode(newNode);
       nodeMap_[key] = newNode;
    }

    // 将该节点移动到最新的位置，当该节点被访问时且存在在缓存中，调用
    // 移除该节点，然后重新插入到链表尾部
    void moveToMostRecent(NodePtr node) 
    {
        removeNode(node);
        insertNode(node);
    }

    // 从链表中移除结点
    // 如果该节点的前向指针和后向指针都不为空，则调整它们的指针
    // 使该节点的前一个节点与下一个节点相连，从而将该节点从链表中断开
    // 同时清除 node->next_ 指针，保证下一个节点的引用计数正确减少，防止内存泄漏
    // 原先的链表结构： prevNode <-> node <-> nextNode
    // 最终形成： prevNode <-> nextNode(只有一个引用计数)
    void removeNode(NodePtr node) 
    {
        if(!node->prev_.expired() && node->next_) 
        {
            auto prev = node->prev_.lock(); // 使用lock()获取shared_ptr
            prev->next_ = node->next_;
            node->next_->prev_ = prev;
            node->next_ = nullptr; // 清空next_指针，彻底断开节点与链表的连接
        }
    }

    // 从尾部插入结点
    // 新尾部节点的后向指针指向哨兵节点，新尾部节点的前向指针指向哨兵节点的前一个节点
    // 哨兵节点的前一个节点的后向指针指向新尾部节点，即改变链表连接；由于后向指针是强引用，因此调用lock()获取shared_ptr
    // 哨兵节点的前向指针指向新尾部节点
    // 原先的链表结构： prevNode <-> dummyTail
    // 最终形成： prevNode <-> newNode <-> dummyTail
    void insertNode(NodePtr node) 
    {
        node->next_ = dummyTail_;
        node->prev_ = dummyTail_->prev_;
        dummyTail_->prev_.lock()->next_ = node; // 使用lock()获取shared_ptr
        dummyTail_->prev_ = node;
    }

    // 驱逐最近最少访问
    // 删除链表头部的第一个真实节点，即最近最少访问的节点
    // 从哈希表中删除该节点的映射关系，让该数值不存在于缓存中
    void evictLeastRecent() 
    {
        NodePtr leastRecent = dummyHead_->next_;
        removeNode(leastRecent);
        nodeMap_.erase(leastRecent->getKey());
    }

private:
    int           capacity_;  // 缓存最大容量
    NodeMap       nodeMap_;   // 哈希表。存储 Key -> Node指针 的映射。用于快速定位节点。
    std::mutex    mutex_;     // 互斥锁，保证线程安全
    NodePtr       dummyHead_; // 虚拟头结点
    NodePtr       dummyTail_; // 虚拟尾结点
};

// LRU优化：Lru-k版本。 通过继承的方式进行再优化
template<typename Key, typename Value>
class KLruKCache : public KLruCache<Key, Value>
{
public:
    // 构造函数调用了基类的构造函数，同时初始化了历史访问记录缓存和k值
    KLruKCache(int capacity, int historyCapacity, int k)
        : KLruCache<Key, Value>(capacity) // 调用基类构造，构造容量为capacity的LRU缓存
        , historyList_(std::make_unique<KLruCache<Key, size_t>>(historyCapacity)) // 历史访问记录缓存大小 复用KLruCache实现
        , k_(k) // 设置k值，即进入缓存队列的阈值
    {}
    // 对基类KLruCache中get的override 由于基类是虚函数get，因此这里也是override，但是没写
    Value get(Key key) 
    {
        // 首先尝试从主缓存获取数据
        Value value{};
        // 由于继承了基类KLruCache，因此可以直接调用基类的get方法
        // 如果没有声明基类方法，则默认调用当前类的方法，导致无限递归
        bool inMainCache = KLruCache<Key, Value>::get(key, value);

        // 历史访问次数的维护
        // 复用基类KLruCache的方法去管理历史访问计数
        size_t historyCount = historyList_->get(key); 
        historyCount++;
        historyList_->put(key, historyCount); // 重新设置访问次数

        // 如果数据在主缓存中，直接返回
        if (inMainCache) 
        {
            return value;
        }

        // 如果数据不在主缓存，但访问次数达到了k次
        if (historyCount >= k_) 
        {
            // 检查是否有历史值记录
            auto it = historyValueMap_.find(key);
            if (it != historyValueMap_.end()) 
            {
                // 有历史值，将其添加到主缓存
                Value storedValue = it->second;
                
                // 从历史记录移除
                historyList_->remove(key);
                historyValueMap_.erase(it);
                
                // 添加到主缓存
                KLruCache<Key, Value>::put(key, storedValue);
                
                return storedValue;
            }
            // 没有历史值记录，无法添加到缓存，返回默认值 -> 发生在高频只访问但未写入的场景 导致historyValueMap_中没有预存值
        }

        // 数据不在主缓存且不满足添加条件，返回默认值
        return value;
    }

    // 对基类KLruCache中put的override 由于基类是虚函数put，因此这里也是override，但是没写
    void put(Key key, Value value) 
    {
        // 检查是否已在主缓存
        Value existingValue{};
        bool inMainCache = KLruCache<Key, Value>::get(key, existingValue);
        // 如果已在主缓存，直接更新 不用考虑外部的k值与历史访问记录内容
        // 历史访问记录只用来保存未进入主缓存的数据
        if (inMainCache) 
        {
            // 已在主缓存，直接更新
            KLruCache<Key, Value>::put(key, value);
            return;
        }
        
        // 获取并更新访问历史
        size_t historyCount = historyList_->get(key);
        historyCount++;
        historyList_->put(key, historyCount);
        
        // 保存值到历史记录映射，供后续get操作使用
        historyValueMap_[key] = value;
        
        // 检查是否达到k次访问阈值
        if (historyCount >= k_) 
        {
            // 达到阈值，添加到主缓存
            historyList_->remove(key);
            historyValueMap_.erase(key);
            KLruCache<Key, Value>::put(key, value);
        }
    }

private:
    int                                     k_; // 进入缓存队列的评判标准
    std::unique_ptr<KLruCache<Key, size_t>> historyList_; // 访问数据历史记录(value为访问次数)
    std::unordered_map<Key, Value>          historyValueMap_; // 存储未达到k次访问的数据值
};

// lru优化：对lru进行分片，提高高并发使用的性能
template<typename Key, typename Value>
class KHashLruCaches
{
public:
    KHashLruCaches(size_t capacity, int sliceNum)
        : capacity_(capacity)
        , sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())
    {
        size_t sliceSize = std::ceil(capacity / static_cast<double>(sliceNum_)); // 获取每个分片的大小
        for (int i = 0; i < sliceNum_; ++i)
        {
            lruSliceCaches_.emplace_back(new KLruCache<Key, Value>(sliceSize)); 
        }
    }

    void put(Key key, Value value)
    {
        // 获取key的hash值，并计算出对应的分片索引
        size_t sliceIndex = Hash(key) % sliceNum_;
        lruSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(Key key, Value& value)
    {
        // 获取key的hash值，并计算出对应的分片索引
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lruSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(Key key)
    {
        Value value;
        memset(&value, 0, sizeof(value));
        get(key, value);
        return value;
    }

private:
    // 将key转换为对应hash值
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc;
        return hashFunc(key);
    }

private:
    size_t                                              capacity_;  // 总容量
    int                                                 sliceNum_;  // 切片数量
    std::vector<std::unique_ptr<KLruCache<Key, Value>>> lruSliceCaches_; // 切片LRU缓存
};

} // namespace KamaCache