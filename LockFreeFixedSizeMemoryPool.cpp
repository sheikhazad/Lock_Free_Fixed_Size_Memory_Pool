#include <cstddef>      // std::size_t
#include <cstdint>      // std::uint64_t
#include <atomic>       // std::atomic
#include <new>          // placement new
#include <type_traits>  // std::aligned_storage
#include <cassert>      // assert

// ========== Cache Line Alignment ========== //
// Use hardware-specific cache line size if available (C++17+)
#ifndef hardware_destructive_interference_size
    #define hardware_destructive_interference_size 64  // 64-byte cache line for modern CPUs
#endif

constexpr static std::size_t CACHE_LINE_SIZE = hardware_destructive_interference_size;

/**
 * @brief Ultra-low-latency lock-free memory pool for fixed-size objects.
 * 
 * This memory pool is preallocated, lock-free, and cache-line optimized.
 * It is designed for HFT or real-time systems where allocation speed and
 * cache behavior are critical.
 * 
 * @tparam T Type of object to allocate
 * @tparam N Number of objects to preallocate
 */
template<typename T, std::size_t N>
class LockFreeFixedSizeMemoryPool {
private:
    // Internal free list node structure (same size as T)
    struct FreeNode {
        FreeNode* next;
    };

    /**
     * @brief Memory buffer to hold N elements of type T
     * Each element is aligned to CACHE_LINE_SIZE to avoid false sharing
     */
    alignas(CACHE_LINE_SIZE)
    T buffer[N];

    //freeList stores always Head of the lock-free free list
    std::atomic<FreeNode*> freeList;

public:
    /**
     * @brief Constructor initializes the free list by linking all blocks
     */
    LockFreeFixedSizeMemoryPool() noexcept {
        FreeNode* head = nullptr;

        // Link all blocks into the free list (in reverse order)
        for (std::size_t i = 0; i < N; ++i) {
            auto* node = reinterpret_cast<FreeNode*>(&buffer[i]);
            node->next = head;
            head = node;
        }

        freeList.store(head, std::memory_order_release);
    }

    /**
     * @brief Allocates a block of memory for one object
     * @return Pointer to uninitialized memory, or nullptr if pool exhausted
     */
    T* allocate() noexcept {
        FreeNode* head = freeList.load(std::memory_order_acquire);
        while (head) {
            FreeNode* next = head->next;

            if (freeList.compare_exchange_weak(head, next, std::memory_order_acq_rel)) {
                return reinterpret_cast<T*>(head);
            }
        }

        return nullptr; // Pool is exhausted
    }

    /**
     * @brief Returns a previously allocated block back to the pool
     * @param ptr Pointer to object previously returned by allocate()
     */
    void deallocate(T* ptr) noexcept {
        auto* node = reinterpret_cast<FreeNode*>(ptr);
        FreeNode* head = freeList.load(std::memory_order_acquire);

        do {
            node->next = head;
        } while (! freeList.compare_exchange_weak(head, node, std::memory_order_acq_rel));
    }

    // Prevent accidental copies
    LockFreeFixedSizeMemoryPool(const LockFreeFixedSizeMemoryPool&) = delete;
    LockFreeFixedSizeMemoryPool& operator=(const LockFreeFixedSizeMemoryPool&) = delete;

};

/*************Usage Example**************/

// Your aligned object for HFT (e.g., Order)
struct alignas(CACHE_LINE_SIZE) Order {
    uint64_t id;
    double price;
    int quantity;

    Order(uint64_t id_, double price_, int qty_)
        : id(id_), price(price_), quantity(qty_) {}
};


int main() {
    // Step 1: Create memory pool with capacity for 1024 Orders
    LockFreeFixedSizeMemoryPool <Order, 1024> pool;

    // Step 2: Allocate raw memory for one Order object
    Order* order = pool.allocate();
    assert(order != nullptr && "Pool exhausted");

    // Step 3: Construct the object in-place using placement new
    new (order) Order(1001, 99.95, 200);

    // Step 4: Use the object
    std::cout << "Order ID: " << order->id
              << ", Price: " << order->price
              << ", Qty: " << order->quantity << "\n";

    // Step 5: Explicitly destroy the object
    order->~Order();

    // Step 6: Return memory to the pool
    pool.deallocate(order);

    return 0;
}
