// Copyright (c) Microsoft Corporation.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// implement atomic wait / notify_one / notify_all

#include <atomic>
#include <cstdint>
#include <new>
#include <thread>

#include <Windows.h>

namespace {

    constexpr std::size_t _Wait_table_size_power = 8;
    constexpr std::size_t _Wait_table_size       = 1 << _Wait_table_size_power;
    constexpr std::size_t _Wait_table_index_mask = _Wait_table_size - 1;

#pragma warning(push)
#pragma warning(disable : 4324) // structure was padded due to alignment specifier
    struct alignas(std::hardware_destructive_interference_size) _Wait_table_entry {
        // Arbitraty variable to wait/notify on if target wariable is not proper atomic for that
        // Size is largest of lock-free to make aliasing problem into hypothetical
        std::atomic<std::uint64_t> _Counter;

        CONDITION_VARIABLE _Condition = CONDITION_VARIABLE_INIT;
        SRWLOCK _Lock                 = SRWLOCK_INIT;
    };
#pragma warning(pop)

    _Wait_table_entry& _Atomic_wait_table_entry(const void* const _Storage) noexcept {
        static _Wait_table_entry wait_table[_Wait_table_size];
        auto index = reinterpret_cast<std::uintptr_t>(_Storage);
        index ^= index >> (_Wait_table_size_power * 2);
        index ^= index >> _Wait_table_size_power;
        return wait_table[index & _Wait_table_index_mask];
    }

    enum _Atomic_spin_phase : std::size_t {
        _Atomic_wait_phase_mask            = 0x0000'000F,
        _Atomic_spin_value_mask            = 0xFFFF'FFF0,
        _Atomic_spin_value_step            = 0x0000'0010,
        _Atomic_wait_phase_init_spin_count = 0x0000'0000,
        _Atomic_wait_phase_spin            = 0x0000'0002,
        _Atomic_wait_phase_wait            = 0x0000'0001,
        _Atomic_wait_phase_wait_indirect   = 0x0000'0004,
    };

    static_assert(_Atomic_unwait_needed == _Atomic_wait_phase_wait);

    inline std::size_t _Atomic_get_spin_count() noexcept {
        static std::size_t constexpr uninitialized_spin_count = (std::numeric_limits<std::size_t>::max)();
        static std::atomic<std::size_t> atomic_spin_count     = uninitialized_spin_count;
        std::size_t result                                    = atomic_spin_count.load(std::memory_order_relaxed);
        if (result == uninitialized_spin_count) {
            result = (std::thread::hardware_concurrency() == 1 ? 0 : 10'000) * _Atomic_spin_value_step;
            atomic_spin_count.store(result, std::memory_order_relaxed);

            // Make sure other thread is likely to get this,
            // as we've done kernel call for that.
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        return result;
    }


    inline bool _Atomic_wait_spin(std::size_t& _Wait_context) noexcept {
        switch (_Wait_context & _Atomic_wait_phase_mask) {
        case _Atomic_wait_phase_init_spin_count: {
            _Wait_context = _Atomic_wait_phase_spin | _Atomic_get_spin_count();
            [[fallthrough]];
        }

        case _Atomic_wait_phase_spin: {
            if ((_Wait_context & _Atomic_spin_value_mask) > 0) {
                _Wait_context -= _Atomic_spin_value_step;
                YieldProcessor();
                return true;
            }
            _Wait_context = _Atomic_wait_phase_wait_indirect;
            break;
        }
        }
        return false;
    }

#if _STL_WIN32_WINNT >= _WIN32_WINNT_WIN8
    constexpr bool _Have_wait_functions() {
        return true;
    }
#define __crtWaitOnAddress       WaitOnAddress
#define __crtWakeByAddressSingle WakeByAddressSingle
#define __crtWakeByAddressAll    WakeByAddressAll

#pragma comment(lib, "Synchronization.lib")

    void _Atomic_wait_fallback(
        [[maybe_unused]] const void* const _Storage, [[maybe_unused]] std::size_t& _Wait_context) noexcept {
        std::terminate();
    }

    void _Atomic_notify_fallback([[maybe_unused]] const void* const _Storage) noexcept {
        std::terminate();
    }

    void _Atomic_unwait_fallback(
        [[maybe_unused]] const void* const _Storage, [[maybe_unused]] std::size_t& _Wait_context) noexcept {}

#else // ^^^ _STL_WIN32_WINNT >= _WIN32_WINNT_WIN8 / _STL_WIN32_WINNT < _WIN32_WINNT_WIN8 vvv
    void _Atomic_wait_fallback(const void* const _Storage, std::size_t& _Wait_context) noexcept {
        switch (_Wait_context & _Atomic_wait_phase_mask) {
        case _Atomic_wait_phase_init_spin_count: {
            _Wait_context = _Atomic_wait_phase_spin | _Atomic_get_spin_count();
            [[fallthrough]];
        }

        case _Atomic_wait_phase_spin: {
            if ((_Wait_context & _Atomic_spin_value_mask) > 0) {
                _Wait_context -= _Atomic_spin_value_step;
                YieldProcessor();
                return;
            }

            _Wait_context = _Atomic_wait_phase_wait;

            auto& entry = _Atomic_wait_table_entry(_Storage);
            ::AcquireSRWLockExclusive(&entry._Lock);
            [[fallthrough]];
        }

        case _Atomic_wait_phase_wait: {
            auto& entry = _Atomic_wait_table_entry(_Storage);
            ::SleepConditionVariableSRW(&entry._Condition, &entry._Lock, INFINITE, 0);
            return;
        }
        }
    }

    void _Atomic_unwait_fallback(const void* const _Storage, std::size_t& _Wait_context) noexcept {
        if ((_Wait_context & _Atomic_wait_phase_wait) != 0) {
            auto& entry = _Atomic_wait_table_entry(_Storage);
            ::ReleaseSRWLockExclusive(&entry._Lock);
        }
    }

    void _Atomic_notify_fallback(const void* const _Storage) noexcept {
        auto& entry = _Atomic_wait_table_entry(_Storage);
        ::AcquireSRWLockExclusive(&entry._Lock);
        ::ReleaseSRWLockExclusive(&entry._Lock);
        ::WakeAllConditionVariable(&entry._Condition);
    }

    struct _Wait_on_address_functions {
        std::atomic<decltype(&::WaitOnAddress)> _Pfn_WaitOnAddress;
        std::atomic<decltype(&::WakeByAddressSingle)> _Pfn_WakeByAddressSingle;
        std::atomic<decltype(&::WakeByAddressAll)> _Pfn_WakeByAddressAll;
        std::atomic<bool> _Initialized;
    };

    const _Wait_on_address_functions& _Get_wait_functions() {
        static _Wait_on_address_functions functions;
        if (!functions._Initialized.load(std::memory_order_relaxed)) {
            HMODULE sync_api_module        = ::GetModuleHandle(TEXT("API-MS-WIN-CORE-SYNCH-L1-2-0.DLL"));
            FARPROC wait_on_address        = ::GetProcAddress(sync_api_module, "WaitOnAddress");
            FARPROC wake_by_address_single = ::GetProcAddress(sync_api_module, "WakeByAddressSingle");
            FARPROC wake_by_address_all    = ::GetProcAddress(sync_api_module, "WakeByAddressAll");

            if (wait_on_address != nullptr && wake_by_address_single != nullptr && wake_by_address_all != nullptr) {
                functions._Pfn_WaitOnAddress.store(
                    reinterpret_cast<decltype(&::WaitOnAddress)>(wait_on_address), std::memory_order_relaxed);
                functions._Pfn_WakeByAddressSingle.store(
                    reinterpret_cast<decltype(&::WakeByAddressSingle)>(wake_by_address_single),
                    std::memory_order_relaxed);
                functions._Pfn_WakeByAddressAll.store(
                    reinterpret_cast<decltype(&::WakeByAddressAll)>(wake_by_address_all), std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
            functions._Initialized.store(true, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        return functions;
    }

    bool _Have_wait_functions() {
        return _Get_wait_functions()._Pfn_WaitOnAddress.load(std::memory_order_relaxed) != nullptr;
    }

    inline BOOL __crtWaitOnAddress(
        volatile VOID* Address, PVOID CompareAddress, SIZE_T AddressSize, DWORD dwMilliseconds) {
        const auto wait_on_address = _Get_wait_functions()._Pfn_WaitOnAddress.load(std::memory_order_relaxed);
        return wait_on_address(Address, CompareAddress, AddressSize, dwMilliseconds);
    }

    inline VOID __crtWakeByAddressSingle(PVOID Address) {
        const auto wake_by_address_single =
            _Get_wait_functions()._Pfn_WakeByAddressSingle.load(std::memory_order_relaxed);
        wake_by_address_single(Address);
    }

    inline VOID __crtWakeByAddressAll(PVOID Address) {
        const auto wake_by_address_all = _Get_wait_functions()._Pfn_WakeByAddressAll.load(std::memory_order_relaxed);
        wake_by_address_all(Address);
    }
#endif //  _STL_WIN32_WINNT >= _WIN32_WINNT_WIN8

} // unnamed namespace

_EXTERN_C
void _CRT_SATELLITE_1 __stdcall __std_atomic_wait_direct(
    const void* _Storage, const void* const _Comparand, const std::size_t _Size, std::size_t& _Wait_context) noexcept {
    if (_Have_wait_functions()) {
        __crtWaitOnAddress(const_cast<volatile void*>(_Storage), const_cast<void*>(_Comparand), _Size, INFINITE);
    } else {
        _Atomic_wait_fallback(_Storage, _Wait_context);
    }
}

void _CRT_SATELLITE_1 __stdcall __std_atomic_notify_one_direct(const void* const _Storage) noexcept {
    if (_Have_wait_functions()) {
        __crtWakeByAddressSingle(const_cast<void*>(_Storage));
    } else {
        _Atomic_notify_fallback(_Storage);
    }
}

void _CRT_SATELLITE_1 __stdcall __std_atomic_notify_all_direct(const void* const _Storage) noexcept {
    if (_Have_wait_functions()) {
        __crtWakeByAddressAll(const_cast<void*>(_Storage));
    } else {
        _Atomic_notify_fallback(_Storage);
    }
}

void _CRT_SATELLITE_1 __stdcall __std_atomic_wait_indirect(
    const void* const _Storage, std::size_t& _Wait_context) noexcept {
    if (_Have_wait_functions()) {
        // Spin here, since spinning inside WaitOnAddress is not helpful in case of change without notification
        if (_Atomic_wait_spin(_Wait_context)) {
            return;
        }

        auto& entry = _Atomic_wait_table_entry(_Storage);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto counter = entry._Counter.load(std::memory_order_relaxed);
        __crtWaitOnAddress(const_cast<volatile std::uint64_t*>(&entry._Counter._Storage._Value), &counter,
            sizeof(entry._Counter._Storage._Value), INFINITE);
    } else {
        _Atomic_wait_fallback(_Storage, _Wait_context);
    }
}

void _CRT_SATELLITE_1 __stdcall __std_atomic_notify_one_indirect(const void* const _Storage) noexcept {
    return __std_atomic_notify_all_indirect(_Storage);
}

void _CRT_SATELLITE_1 __stdcall __std_atomic_notify_all_indirect(const void* const _Storage) noexcept {
    if (_Have_wait_functions()) {
        auto& entry = _Atomic_wait_table_entry(_Storage);
        entry._Counter.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        __crtWakeByAddressAll(&entry._Counter._Storage._Value);
    } else {
        _Atomic_notify_fallback(_Storage);
    }
}

void _CRT_SATELLITE_1 __stdcall __std_atomic_unwait_direct(
    const void* const _Storage, std::size_t& _Wait_context) noexcept {
    _Atomic_unwait_fallback(_Storage, _Wait_context);
}

void _CRT_SATELLITE_1 __stdcall __std_atomic_unwait_indirect(
    const void* const _Storage, std::size_t& _Wait_context) noexcept {
    _Atomic_unwait_fallback(_Storage, _Wait_context);
}
_END_EXTERN_C
