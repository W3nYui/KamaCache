#pragma once

#include "../KICachePolicy.h"
#include "KArcLruPart.h"
#include "KArcLfuPart.h"
#include <memory>

namespace KamaCache 
{

//使用泛型接口的方式，利用模板类与继承，同时补充定义函数，完成了接口的覆写,同时遵照组合优于继承，组合了LRU，LFU，最后才在ARC的接口继承了虚函数需要的外部接口，实现调用。
template<typename Key, typename Value> 
class KArcCache : public KICachePolicy<Key, Value> 
{
public:
    /**
     * @brief 构造函数 构造Arc内部的LRU与LFU部分 两者初始缓存大小为10 同时晋升阈值为2。
     * 默认带参构造，如果没有传入参数，则以 capacity = 10，transformThreshold = 2的数据进行构造。
     * 且采用 explicit 显式构造，定义必须KArcCache<> cache(50);来完成单参构造。
     * @param capacity 
     * @param transformThreshold 
     */
    explicit KArcCache(size_t capacity = 10, size_t transformThreshold = 2)
        : capacity_(capacity)
        , transformThreshold_(transformThreshold)
        , lruPart_(std::make_unique<ArcLruPart<Key, Value>>(capacity, transformThreshold))
        , lfuPart_(std::make_unique<ArcLfuPart<Key, Value>>(capacity, transformThreshold))
    {
        // lruPart_ = std::make_unique<ArcLruPart<Key, Value>>(capacity, transformThreshold);
        // lfuPart_ = std::make_unique<ArcLfuPart<Key, Value>>(capacity, transformThreshold);
    }

    ~KArcCache() override = default;

    /**
     * @brief 写入函数
     * 
     * @param key 
     * @param value 
     */
    void put(Key key, Value value) override 
    {
        checkGhostCaches(key); // 触发幽灵命中 更新缓存空间大小

        // 检查 LFU 部分是否存在该键
        bool inLfu = lfuPart_->contain(key);
        // 更新/放入 LRU 部分缓存
        lruPart_->put(key, value);
        // 如果 LFU 部分存在该键，则更新 LFU 部分 对于这一部分的代码我认为有问题，卡哥的思路是将LRU的内容升级到LFU中，并一起更新
        // 但是这种一起更新的行为没有维护ARC中LFU与LRU的互斥空间问题。如果LFU数据存在，但是触发了LRU的幽灵命中，那么会导致LFU的空间反而被缩减。
        if (inLfu) 
        {
            lfuPart_->put(key, value);
        }
    }

    /**
     * @brief 访问函数
     * 
     * @param key 
     * @param value 
     * @return true 
     * @return false 
     */
    bool get(Key key, Value& value) override 
    {
        checkGhostCaches(key);

        // 晋升函数
        bool shouldTransform = false;
        if (lruPart_->get(key, value, shouldTransform)) // 如果LRU中有，判断完是否晋升后直接返回
        {
            if (shouldTransform) 
            {
                lfuPart_->put(key, value);
            }
            return true;
        }
        return lfuPart_->get(key, value);   // 如果LRU中没有再判断LFU 最后返回
    }

    Value get(Key key) override  // 复用
    {
        Value value{};
        get(key, value);
        return value;
    }

private:
    /**
     * @brief 利用幽灵缓存动态调整缓存空间大小 以面对不同情况
     * 
     * @param key 
     * @return true 
     * @return false 
     */
    bool checkGhostCaches(Key key) 
    {
        bool inGhost = false;
        // 如果在 LRU 的幽灵区命中了 -> 说明 LRU 空间太小了 
        if (lruPart_->checkGhost(key)) 
        {
            // 削减 LFU 的容量，增加 LRU 的容量 如果可以的话 decreaseCapacity会返回缩减是否成功
            if (lfuPart_->decreaseCapacity()) 
            {
                lruPart_->increaseCapacity();
            }
            inGhost = true;
        } 
        // 反之，如果在 LFU 的幽灵区命中 -> 说明 LFU 空间太小
        else if (lfuPart_->checkGhost(key)) 
        {
            // 削减 LRU，增加 LFU 如果可以的话 decreaseCapacity会返回缩减是否成功
            if (lruPart_->decreaseCapacity()) 
            {
                lfuPart_->increaseCapacity();
            }
            inGhost = true;
        }
        return inGhost;
    }

private:
    size_t capacity_; // 单个缓存大小
    size_t transformThreshold_;
    std::unique_ptr<ArcLruPart<Key, Value>> lruPart_;
    std::unique_ptr<ArcLfuPart<Key, Value>> lfuPart_;
};

} // namespace KamaCache