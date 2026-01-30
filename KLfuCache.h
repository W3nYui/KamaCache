#pragma once

#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <climits>
#include <algorithm>

#include "KICachePolicy.h"

namespace KamaCache
{
// 前向声明
template<typename Key, typename Value> class KLfuCache;

template<typename Key, typename Value>
/**
 * @brief 频次列表
 * 
 */
class FreqList
{
private:
    struct Node
    {
        int freq; // 访问频次
        Key key;
        Value value;
        std::weak_ptr<Node> pre; // 上一结点改为weak_ptr打破循环引用
        std::shared_ptr<Node> next;

        Node() 
        : freq(1), next(nullptr) {}
        Node(Key key, Value value) 
        : freq(1), key(key), value(value), next(nullptr) {}
    };

    using NodePtr = std::shared_ptr<Node>;
    int freq_; // 访问频率
    NodePtr head_; // 假头结点
    NodePtr tail_; // 假尾结点

public:
    // 构造函数 初始化频率值以及头尾结点 
    explicit FreqList(int n) 
     : freq_(n) 
    {
      head_ = std::make_shared<Node>();
      tail_ = std::make_shared<Node>();
      head_->next = tail_;
      tail_->pre = head_;
    }

    bool isEmpty() const
    {
      return head_->next == tail_;
    }

    // 提那家结点管理方法
    void addNode(NodePtr node) 
    {
        // 防止空指针异常
        // head_与tail_的判断避免了容器未正确初始化时的异常/move语义下的错误操作
        if (!node || !head_ || !tail_) 
            return;
        // 从尾部插入结点 同时用到了shared_ptr和weak_ptr的知识 需要lock调用shared_ptr

        node->pre = tail_->pre;
        node->next = tail_;
        tail_->pre.lock()->next = node; // 使用lock()获取shared_ptr
        tail_->pre = node;
    }

    void removeNode(NodePtr node)
    {
        // 防止空指针异常 与addNode功能类似
        if (!node || !head_ || !tail_)
            return;
        // !node->next 防止该节点已被移除(包括移除了tail节点或其他节点)
        // node->pre.expired() 判断前向指针是否为空，因为前向指针是weak_ptr类型，因此不能用 !node->pre 而需要调用expired()
        // 两者都是为了保证节点在链表中的有效性
        if (node->pre.expired() || !node->next) 
            return;

        auto pre = node->pre.lock(); // 使用lock()获取shared_ptr
        pre->next = node->next;
        node->next->pre = pre;
        node->next = nullptr; // 确保显式置空next指针，彻底断开节点与链表的连接
    }

    NodePtr getFirstNode() const { return head_->next; }
    
    friend class KLfuCache<Key, Value>;
};

template <typename Key, typename Value>
class KLfuCache : public KICachePolicy<Key, Value>
{
public:
    // 由于Node是类模板FreqList的私有成员结构体，因此需要使用typename关键字来告诉编译器Node是一个类型
    // 如果不加typename，编译器会将Node解释为一个静态成员或其他非类型实体，导致编译错误
    using Node = typename FreqList<Key, Value>::Node;
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;
    // 构造函数 定义缓存容量，最大访问频次，初始化最小访问频次、平均访问频次与当前访问所有缓存次数总和
    KLfuCache(int capacity, int maxAverageNum = 1000000)
    : capacity_(capacity), minFreq_(INT8_MAX), maxAverageNum_(maxAverageNum),
      curAverageNum_(0), curTotalNum_(0) 
    {}
    // 虚函数默认析构 从下至上
    ~KLfuCache() override = default;
    // 插入并更新
    void put(Key key, Value value) override
    {
        // 缓存容量维护
        if (capacity_ == 0)
            return;
        // Map锁
        std::lock_guard<std::mutex> lock(mutex_);
        // 直接通过 key -> Node 的映射表完成O(1)查找
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // 重置其value值
            // 这句话的翻译是：it找到的是unordered_map<Key, NodePtr>
            // 因此需要找到Node指针，即it -> second，最后更改指针中结构体包含的value变量
            it->second->value = value;
            // 找到了直接调整就好了，不用再去get中再找一遍，但其实影响不大
            getInternal(it->second, value); // 这里查找一次 其实就是复用了其中的增加频次的晋升功能
            return;
        }
        // 否则触发放入函数
        putInternal(key, value);
    }

    // value值为传出参数
    bool get(Key key, Value& value) override
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = nodeMap_.find(key);
      if (it != nodeMap_.end())
      {
          getInternal(it->second, value);
          return true;
      }

      return false;
    }

    Value get(Key key) override
    {
      Value value;
      get(key, value);
      return value;
    }

    // 清空缓存,回收资源
    void purge()
    {
      nodeMap_.clear();
      freqToFreqList_.clear();
    }

private:
    void putInternal(Key key, Value value); // 添加缓存
    void getInternal(NodePtr node, Value& value); // 获取缓存

    void kickOut(); // 移除缓存中的过期数据

    void removeFromFreqList(NodePtr node); // 从频率列表中移除节点
    void addToFreqList(NodePtr node); // 添加到频率列表

    void addFreqNum(); // 增加平均访问等频率
    void decreaseFreqNum(int num); // 减少平均访问等频率
    void handleOverMaxAverageNum(); // 处理当前平均访问频率超过上限的情况
    void updateMinFreq();

private:
    int                                            capacity_; // 缓存容量
    int                                            minFreq_; // 最小访问频次(用于找到最小访问频次结点)
    int                                            maxAverageNum_; // 最大平均访问频次
    int                                            curAverageNum_; // 当前平均访问频次
    int                                            curTotalNum_; // 当前访问所有缓存次数总数 
    std::mutex                                     mutex_; // 互斥锁
    NodeMap                                        nodeMap_;       // key 到 缓存节点的映射 实现O(1)索引节点
    std::unordered_map<int, FreqList<Key, Value>*> freqToFreqList_;// 访问频次到该频次链表的映射 实现频率分层
};

template<typename Key, typename Value>
void KLfuCache<Key, Value>::getInternal(NodePtr node, Value& value)
{
    // 找到之后需要将其从低访问频次的链表中删除，并且添加到+1的访问频次链表中，
    // 获得目标数值
    value = node->value;
    // 从原有访问频次的链表中删除节点
    removeFromFreqList(node); 
    // 提升其频次
    node->freq++;
    // 插入新的频次列表中
    addToFreqList(node);
    // 如果当前node的访问频次如果等于minFreq+1，并且其前驱链表为空，则说明
    // freqToFreqList_[node->freq - 1]链表因node的迁移已经空了，需要更新最小访问频次
    if (node->freq - 1 == minFreq_ && freqToFreqList_[node->freq - 1]->isEmpty()) // 检验前一频次链表是否为空 避免内存爆炸
        minFreq_++;

    // 总访问频次和当前平均访问频次都随之增加
    addFreqNum();
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::putInternal(Key key, Value value)
{   
    // 如果不在缓存中，则需要判断缓存是否已满
    if (nodeMap_.size() == capacity_)
    {
        // 缓存已满，删除最少最不常访问的结点，更新当前平均访问频次和总访问频次
        kickOut();
    }
    
    // 创建新结点，将新结点添加进入，更新最小访问频次
    NodePtr node = std::make_shared<Node>(key, value); // 初始化一个新的节点 其节点的频次为1
    nodeMap_[key] = node;
    addToFreqList(node); // 添加到频次链表
    addFreqNum();        // 增加访问频次
    minFreq_ = std::min(minFreq_, 1); // 由于是加入新节点 因此一定是仅有1次访问，因此更新最小访问频次
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::kickOut()
{
    // 由于一直维护最小访问频次 因此可以O(1)的直接找到节点 直接删除头结点：即最小访问频次下的最久未访问节点
    NodePtr node = freqToFreqList_[minFreq_]->getFirstNode();
    removeFromFreqList(node);
    nodeMap_.erase(node->key);
    decreaseFreqNum(node->freq);
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::removeFromFreqList(NodePtr node)
{
    // 检查结点是否为空
    if (!node) 
        return;
    // 获得频次 并从对应的频次列表中删除
    auto freq = node->freq;
    freqToFreqList_[freq]->removeNode(node);
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::addToFreqList(NodePtr node)
{
    // 检查结点是否为空
    if (!node) 
        return;

    // 添加进入相应的频次链表前需要判断该频次链表是否存在
    auto freq = node->freq;
    if (freqToFreqList_.find(node->freq) == freqToFreqList_.end())
    {
        // 不存在则在频次哈希表中创建该频次的链表
        freqToFreqList_[node->freq] = new FreqList<Key, Value>(node->freq);
    }

    freqToFreqList_[freq]->addNode(node);
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::addFreqNum()
{
    curTotalNum_++;
    if (nodeMap_.empty()) // nodeMap存了所有Node指针 为空则没有任何访问
        curAverageNum_ = 0;
    else
        curAverageNum_ = curTotalNum_ / nodeMap_.size();

    if (curAverageNum_ > maxAverageNum_) // 出现访问爆炸的情况
    {
       handleOverMaxAverageNum();
    }
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::decreaseFreqNum(int num)
{
    // 减少平均访问频次和总访问频次
    curTotalNum_ -= num;
    if (nodeMap_.empty())
        curAverageNum_ = 0;
    else
        curAverageNum_ = curTotalNum_ / nodeMap_.size();
}

template<typename Key, typename Value>
/**
 * @brief 处理当实时平均访问频率超过上限的访问爆炸情形
 */
void KLfuCache<Key, Value>::handleOverMaxAverageNum()
{
    if (nodeMap_.empty()) // 避免误触发
        return;

    // 当前平均访问频次已经超过了最大平均访问频次，所有结点的访问频次- (maxAverageNum_ / 2)
    for (auto it = nodeMap_.begin(); it != nodeMap_.end(); ++it) // 对访问哈希表的所有节点进行频率衰减 即全员降级
    {
        // 检查结点是否为空
        if (!it->second)
            continue;

        NodePtr node = it->second;

        // 先从当前频率列表中移除
        removeFromFreqList(node);

        // 减少频率
        node->freq -= maxAverageNum_ / 2;
        if (node->freq < 1) node->freq = 1;

        // 添加到新的频率列表
        addToFreqList(node);
    }

    // 更新最小频率
    updateMinFreq();
}

template<typename Key, typename Value>
/**
 * @brief 更新最小频次
 * 
 */
void KLfuCache<Key, Value>::updateMinFreq() 
{
    // 初始化为一个不可能达到的大数
    minFreq_ = INT8_MAX;
    for (const auto& pair : freqToFreqList_) 
    {
        if (pair.second && !pair.second->isEmpty()) 
        {
            minFreq_ = std::min(minFreq_, pair.first);
        }
    }
    // 仅在for()逻辑一次也未执行时达到，即缓存为空
    // 在这种情况下，将其重置为 1 是最安全的选择。因为下一个插入的新节点频率必然是 1。这样可以保证系统状态的连贯性。
    if (minFreq_ == INT8_MAX) 
        minFreq_ = 1;
}

// 并没有牺牲空间换时间，他是把原有缓存大小进行了分片。
template<typename Key, typename Value>
class KHashLfuCache
{
public:
    /**
     * @brief 构造函数
     * 
     * @param capacity 缓存总容量
     * @param sliceNum 定义的哈希分片数
     * @param maxAverageNum 每个分片的最大平均访问频次 用于全员降级与上限保护
     */
    KHashLfuCache(size_t capacity, int sliceNum, int maxAverageNum = 10)
        : sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())
        , capacity_(capacity)
    {
        // 计算每个lfu分片的容量 向上取整
        size_t sliceSize = std::ceil(capacity_ / static_cast<double>(sliceNum_));
        // 初始化分片内容 并加入数组
        for (int i = 0; i < sliceNum_; ++i)
        {
            lfuSliceCaches_.emplace_back(new KLfuCache<Key, Value>(sliceSize, maxAverageNum));
        }
    }
    // 定义为KHashLfuCache的接口，而不是继承，在调用的同时再触发对应分片的put函数，让该分片自动管理内存。
    // 调用对应分片的put函数
    void put(Key key, Value value)
    {
        // 根据key找出对应的lfu分片
        size_t sliceIndex = Hash(key) % sliceNum_;
        lfuSliceCaches_[sliceIndex]->put(key, value);
    }
    // 调用对应分片的get函数
    bool get(Key key, Value& value)
    {
        // 根据key找出对应的lfu分片
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lfuSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(Key key)
    {
        Value value;
        get(key, value);
        return value;
    }

    // 清除缓存
    void purge()
    {
        for (auto& lfuSliceCache : lfuSliceCaches_)
        {
            lfuSliceCache->purge();
        }
    }

private:
    // 将key计算成对应哈希值
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc;
        return hashFunc(key);
    }

private:
    size_t capacity_; // 缓存总容量
    int sliceNum_; // 缓存分片数量
    std::vector<std::unique_ptr<KLfuCache<Key, Value>>> lfuSliceCaches_; // 缓存lfu分片容器
};

} // namespace KamaCache

