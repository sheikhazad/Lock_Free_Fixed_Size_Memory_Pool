#include <cstddef>      // std::size_t
#include <cstdint>      // std::uint64_t
#include <atomic>       // std::atomic
#include <new>          // placement new
#include <type_traits>  // std::aligned_storage
#include <cassert>      // assert
#include <array>        // std::array
#include <iostream>     // std::cout
#include <thread>       // std::thread::id

// ========== Cache Line Alignment ========== //
#ifndef hardware_destructive_interference_size
    #define hardware_destructive_interference_size 64  // 64-byte cache line for modern CPUs
#endif

constexpr static std::size_t CACHE_LINE_SIZE = hardware_destructive_interference_size;

/**
 * @brief Ultra-low-latency lock-free memory pool for fixed-size objects.
 * 
 * This memory pool is **preallocated, lock-free, and cache-line optimized**.
 * Designed for **HFT or real-time systems**, where allocation speed and
 * cache behavior are critical.
 * 
 * @tparam T Type of object to allocate
 * @tparam N Number of objects to preallocate
 */
template<typename T, std::size_t N>
class LockFreeFixedSizeMemoryPool {
private:
    struct FreeNode {
        FreeNode* next;
    };

    alignas(CACHE_LINE_SIZE) std::byte buffer[N * sizeof(T)];
    //If freeList is accessed heavily, align it to CACHE_LINE_SIZE to avoid contention:
    alignas(CACHE_LINE_SIZE) std::atomic<FreeNode*> freeList;

    // ========== Thread-Local Caching ========== //
    static thread_local FreeNode* localCacheHead = nullptr; 


public:
    constexpr LockFreeFixedSizeMemoryPool() noexcept {
        FreeNode* head = nullptr;
        //Optimize Free List Initialization**
        //Reverse-order linking is fine, but forward linking might improve cache locality:
        //for (std::size_t i = 0; i < N; ++i) {
            //auto* node = reinterpret_cast<FreeNode*>(&buffer[i * sizeof(T)]);
        for (std::size_t i = N; i > 0; --i) {
            auto* node = reinterpret_cast<FreeNode*>(&buffer[(i-1)*sizeof(T)]);
            node->next = head;
            head = node;
        }
        freeList.store(head, std::memory_order_release);
    }

    /**
     * @brief Allocates memory for one object.
     * @return Pointer to uninitialized memory.
     * 
     * Uses **thread-local cache** for faster allocations.
     * If exhausted, falls back to **dynamic memory allocation**.
     */
    T* allocate() noexcept {
        if (localCacheHead) {
           FreeNode* cached = localCacheHead;
           localCacheHead = cached->next;
           return reinterpret_cast<T*>(cached);
        }

        FreeNode* head = freeList.load(std::memory_order_acquire);
        while (head) {
            FreeNode* next = head->next;
            if (freeList.compare_exchange_weak(head, next, std::memory_order_acq_rel)) {
                return reinterpret_cast<T*>(head);
            }
        }

        // ========= Fallback: Dynamic Allocation ========= //
        std::cerr << "Memory pool exhausted! Falling back to dynamic allocation.\n";
        return reinterpret_cast<T*>(new std::byte[sizeof(T)]);
    }

    /**
     * @brief Deallocates memory, returning it back to the pool.
     * @param ptr Pointer to memory previously allocated by `allocate()`.
     * 
     * **Handles dynamically allocated fallback memory separately**.
     */
    void deallocate(T* ptr) noexcept {
        if (!ptr) return;

        // Check if memory came from the fallback dynamic allocation
        if (reinterpret_cast<std::byte*>(ptr) < buffer.data() ||
            reinterpret_cast<std::byte*>(ptr) >= buffer.data() + buffer.size()) {
            delete[] reinterpret_cast<std::byte*>(ptr);  // Clean up fallback allocation
            return;
        }

        // Return object to thread-local cache for faster reuse
        auto* node = reinterpret_cast<FreeNode*>(ptr);
         node->next = localCacheHead;
         localCacheHead = node;
    }

    LockFreeFixedSizeMemoryPool(const LockFreeFixedSizeMemoryPool&) = delete;
    LockFreeFixedSizeMemoryPool& operator=(const LockFreeFixedSizeMemoryPool&) = delete;
};

template<typename T, std::size_t N>
thread_local typename LockFreeFixedSizeMemoryPool<T, N>::FreeNode* LockFreeFixedSizeMemoryPool<T, N>::localCacheHead = nullptr;

/************* Usage Example **************/

struct alignas(CACHE_LINE_SIZE) Order {
    uint64_t id;
    double price;
    int quantity;

    Order(uint64_t id_, double price_, int qty_)
        : id(id_), price(price_), quantity(qty_) {}

    void print() const {
        std::cout << "Order ID: " << id
                  << ", Price: " << price
                  << ", Qty: " << quantity << "\n";
    }
};

int main() {
    try {
        LockFreeFixedSizeMemoryPool<Order, 1024> pool;

        Order* order1 = pool.allocate();
        new (order1) Order(1001, 99.95, 200);
        order1->print();

        Order* order2 = pool.allocate();
        new (order2) Order(1002, 101.25, 150);
        order2->print();

        order1->~Order();
        pool.deallocate(order1);

        order2->~Order();
        pool.deallocate(order2);

        // **Stress test exhaustion**
        for (int i = 0; i < 1100; ++i) {
            Order* order = pool.allocate();
            new (order) Order(i, 100.0 + i, i * 10);
            order->print();
            order->~Order();
            pool.deallocate(order);
        }

    } catch (const std::bad_alloc& e) {
        std::cerr << "Fatal memory allocation failure: " << e.what() << "\n";
        return -1;
    }

    return 0;
}
