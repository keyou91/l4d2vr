#pragma once
#include "vr_pistol_detachment.h"
#include <array>
#include <algorithm>

namespace l4d2vr_pistol
{
    struct AmmoSnapshot
    {
        std::uintptr_t owner = 0u, weapon = 0u;
        unsigned ownerSerial = 0u, weaponSerial = 0u;
        int clip = -1;
        bool dual = false;
        int capacity = 15;
        bool Valid() const { return owner && weapon && capacity > 0 && capacity <= 15 &&
            clip >= 0 && clip <= (dual ? capacity * 2 : capacity); }
        bool SameGun(const AmmoSnapshot& other) const
        {
            return owner == other.owner && ownerSerial == other.ownerSerial &&
                weapon == other.weapon && weaponSerial == other.weaponSerial;
        }
    };
    inline Hand Opposite(Hand hand)
    { return hand == Hand::Right ? Hand::Left : hand == Hand::Left ? Hand::Right : Hand::None; }
    enum class MagazineAction : unsigned { Eject = 0u, Insert = 1u, Reinsert = 2u, Cycle = 3u };
    struct MagazineState
    {
        unsigned physical = 0u, attached = 0u, chambered = 0u;
        int rightDetached = 0, leftDetached = 0;
    };
    struct MagazineResult
    {
        int clip = 0, reserve = 0, removed = 0, added = 0, released = 0;
        MagazineState state{};
    };

    // Native ammunition remains authoritative. A partial pair without a known
    // pickup partition uses the existing balanced split, never invented rounds.
    class AmmoLedger
    {
    public:
        void Reset() { *this = {}; }
        bool Observe(const AmmoSnapshot& snapshot)
        {
            if (!snapshot.Valid()) return false;
            Scope(snapshot);
            Entry& entry = Find(snapshot);
            if (entry.dual != snapshot.dual || entry.capacity != snapshot.capacity || entry.right + entry.left != snapshot.clip)
            {
                const bool wasDual = entry.dual;
                entry.magazines = {};
                entry.dual = snapshot.dual;
                entry.capacity = snapshot.capacity;
                entry.exact = !snapshot.dual || snapshot.clip == 0 || snapshot.clip == snapshot.capacity * 2;
                if (snapshot.dual)
                { entry.right = snapshot.clip / 2; entry.left = snapshot.clip - entry.right; }
                else
                {
                    if (wasDual) entry.singleHand = Hand::Right;
                    entry.right = entry.singleHand == Hand::Right ? snapshot.clip : 0;
                    entry.left = entry.singleHand == Hand::Left ? snapshot.clip : 0;
                }
            }
            return true;
        }
        bool Single(const AmmoSnapshot& snapshot, Hand hand)
        {
            if (!snapshot.Valid() || snapshot.dual || Opposite(hand) == Hand::None) return false;
            Scope(snapshot);
            Entry& previous = Find(snapshot);
            auto retained = previous.magazines;
            const Hand previousHand = previous.dual ? hand : previous.singleHand;
            const bool keepPhysical = previous.exact && previous.capacity == snapshot.capacity &&
                ((previous.dual && (hand == Hand::Right ? previous.right : previous.left) == snapshot.clip) ||
                    (!previous.dual && previous.right + previous.left == snapshot.clip));
            if (!Observe(snapshot)) return false;
            Entry& entry = Find(snapshot);
            entry.singleHand = hand;
            entry.right = hand == Hand::Right ? snapshot.clip : 0;
            entry.left = hand == Hand::Left ? snapshot.clip : 0;
            entry.exact = true;
            entry.magazines = {};
            const unsigned oldBit = Bit(previousHand);
            if (keepPhysical && (retained.physical & oldBit) != 0u)
            {
                const unsigned bit = Bit(hand);
                entry.magazines.physical = bit;
                entry.magazines.attached = (retained.attached & oldBit) != 0u ? bit : 0u;
                entry.magazines.chambered = (retained.chambered & oldBit) != 0u ? bit : 0u;
                const int detached = previousHand == Hand::Right ? retained.rightDetached : retained.leftDetached;
                if (hand == Hand::Right) entry.magazines.rightDetached = detached;
                else entry.magazines.leftDetached = detached;
            }
            return true;
        }
        bool Joined(const AmmoSnapshot& pair, int held, int incoming, Hand pickup)
        {
            int total = -1;
            if (!pair.dual || !pair.Valid() || !JoinAmmo(held, incoming, total) || held > pair.capacity ||
                incoming > pair.capacity || pair.clip != total ||
                (pickup != Hand::None && Opposite(pickup) == Hand::None)) return false;
            Scope(pair);
            Entry& entry = Find(pair);
            const Hand heldHand = pickup == Hand::None ? entry.singleHand : Opposite(pickup);
            entry.right = heldHand == Hand::Right ? held : incoming;
            entry.left = heldHand == Hand::Left ? held : incoming;
            entry.dual = entry.exact = true;
            entry.capacity = pair.capacity;
            entry.magazines = {};
            return true;
        }
        bool Shot(const AmmoSnapshot& before, const AmmoSnapshot& after, int command, Hand hand,
            int rounds = 1, unsigned ordinal = 0u)
        {
            if (!before.Valid() || !after.Valid() || !before.SameGun(after) || before.dual != after.dual ||
                before.capacity != after.capacity ||
                command <= 0 || Opposite(hand) == Hand::None || rounds <= 0 || rounds > before.capacity ||
                before.clip - after.clip != rounds)
                return false;
            Scope(before);
            Entry& entry = Find(before);
            if (command < entry.lastShot || (command == entry.lastShot && ordinal <= entry.lastOrdinal))
            { Observe(after); return false; }
            Observe(before);
            entry.lastShot = command;
            entry.lastOrdinal = ordinal;
            if (!before.dual)
            {
                if (entry.magazines.physical && entry.singleHand != hand) { Observe(after); return false; }
                entry.singleHand = hand;
                entry.right = hand == Hand::Right ? after.clip : 0;
                entry.left = hand == Hand::Left ? after.clip : 0;
                if ((entry.magazines.physical & Bit(hand)) != 0u)
                {
                    if ((entry.magazines.attached & Bit(hand)) != 0u && after.clip > 0)
                        entry.magazines.chambered |= Bit(hand);
                    else entry.magazines.chambered &= ~Bit(hand);
                }
                entry.exact = true;
                return true;
            }
            int& selected = hand == Hand::Right ? entry.right : entry.left;
            if (!entry.exact || selected < rounds) { Observe(after); return false; }
            selected -= rounds;
            if (entry.magazines.physical)
            {
                const unsigned bit = Bit(hand);
                if ((entry.magazines.attached & bit) != 0u && selected > 0) entry.magazines.chambered |= bit;
                else entry.magazines.chambered &= ~bit;
            }
            return entry.right + entry.left == after.clip;
        }
        bool ObserveFire(const AmmoSnapshot& before, const AmmoSnapshot& after, int command, Hand hand,
            unsigned firstBullet, unsigned lastBullet)
        {
            if (!before.Valid() || !after.Valid() || !before.SameGun(after)) return false;
            if (lastBullet > firstBullet && lastBullet - firstBullet <= static_cast<unsigned>(before.capacity) &&
                Shot(before, after, command, hand, static_cast<int>(lastBullet - firstBullet), firstBullet + 1u))
                return true;
            Observe(after);
            return false;
        }
        bool Split(const AmmoSnapshot& pair, Hand drop, AmmoSplit& result, bool& exact)
        {
            exact = false;
            if (!pair.dual || Opposite(drop) == Hand::None || !Observe(pair)) return false;
            Entry& entry = Find(pair);
            if (!entry.exact) return SplitAmmo(pair.clip, result);
            result = drop == Hand::Right ? AmmoSplit{entry.left, entry.right} : AmmoSplit{entry.right, entry.left};
            exact = true;
            return result.retained + result.dropped == pair.clip;
        }
        bool Counts(const AmmoSnapshot& snapshot, int& right, int& left)
        {
            right = left = 0;
            if (!Observe(snapshot)) return false;
            Entry& entry = Find(snapshot);
            if (!entry.exact) return false;
            right = entry.right; left = entry.left;
            return true;
        }
        bool MagazineInfo(const AmmoSnapshot& snapshot, MagazineState& result)
        {
            if (!Observe(snapshot)) return false;
            const auto& entry = Find(snapshot);
            if (!entry.exact) return false;
            result = entry.magazines; return true;
        }
        bool RelinquishPhysical(const AmmoSnapshot& snapshot)
        {
            if (!snapshot.Valid() || snapshot.owner != m_Owner || snapshot.ownerSerial != m_Serial)
                return false;
            for (auto& entry : m_Entries)
            {
                if (entry.weapon != snapshot.weapon || entry.serial != snapshot.weaponSerial) continue;
                if (entry.dual != snapshot.dual || entry.capacity != snapshot.capacity ||
                    entry.right + entry.left != snapshot.clip || entry.magazines.physical == 0u)
                    return false;
                // Native reload takes over only this gun's mechanical ownership.
                // Keep native rounds, the known hand partition and other guns.
                entry.magazines = {};
                return true;
            }
            return false;
        }
        bool BlocksUnchambered(const AmmoSnapshot& snapshot, Hand hand)
        {
            MagazineState state{};
            return Bit(hand) && MagazineInfo(snapshot, state) &&
                (state.physical & Bit(hand)) != 0u && (state.chambered & Bit(hand)) == 0u;
        }
        template<class Writer> bool Magazine(const AmmoSnapshot& snapshot, Hand hand, MagazineAction action,
            int reserve, bool infiniteReserve, Writer writer, MagazineResult& result)
        {
            if (!Bit(hand) || reserve < 0 || reserve > 5000 || !Observe(snapshot)) return false;
            auto& entry = Find(snapshot);
            if (!entry.exact || (!snapshot.dual && entry.singleHand != hand)) return false;
            Entry candidate = entry;
            const unsigned bit = Bit(hand);
            if (!candidate.magazines.physical)
            {
                candidate.magazines.attached = snapshot.dual ? 3u : bit;
                candidate.magazines.chambered = (candidate.right > 0 ? 1u : 0u) | (candidate.left > 0 ? 2u : 0u);
            }
            int& rounds = hand == Hand::Right ? candidate.right : candidate.left;
            int& detached = hand == Hand::Right ? candidate.magazines.rightDetached : candidate.magazines.leftDetached;
            int nextReserve = reserve, removed = 0, added = 0, released = 0;
            switch (action)
            {
            case MagazineAction::Eject:
                if ((candidate.magazines.attached & bit) == 0u) return false;
                removed = rounds - ((candidate.magazines.chambered & bit) != 0u ? 1 : 0);
                if (removed < 0) return false;
                rounds -= removed; detached = removed;
                candidate.magazines.attached &= ~bit;
                break;
            case MagazineAction::Insert:
            case MagazineAction::Reinsert:
                if ((candidate.magazines.attached & bit) != 0u) return false;
                added = action == MagazineAction::Reinsert ? detached :
                    std::min(snapshot.capacity - rounds, infiniteReserve ? snapshot.capacity : reserve);
                if (added < 0 || added > snapshot.capacity - rounds ||
                    (action == MagazineAction::Insert && added == 0)) return false;
                rounds += added;
                if (action == MagazineAction::Insert) released = detached;
                if (action == MagazineAction::Insert && !infiniteReserve) nextReserve -= added;
                detached = 0; candidate.magazines.attached |= bit;
                break;
            case MagazineAction::Cycle:
                if ((candidate.magazines.attached & bit) == 0u ||
                    (candidate.magazines.chambered & bit) != 0u || rounds <= 0) return false;
                candidate.magazines.chambered |= bit;
                break;
            default: return false;
            }
            candidate.magazines.physical |= bit;
            const int total = candidate.right + candidate.left;
            if (total < 0 || total > snapshot.capacity * (snapshot.dual ? 2 : 1) ||
                !writer(total, nextReserve)) return false;
            entry = candidate;
            result = {total, nextReserve, removed, added, released, candidate.magazines};
            return true;
        }
    private:
        struct Entry
        {
            std::uintptr_t weapon = 0u;
            unsigned serial = 0u;
            unsigned lastOrdinal = 0u;
            int right = 0, left = 0, lastShot = 0, capacity = 15;
            bool dual = false, exact = true;
            Hand singleHand = Hand::Right;
            MagazineState magazines{};
        };
        void Scope(const AmmoSnapshot& snapshot)
        {
            if (snapshot.owner != m_Owner || snapshot.ownerSerial != m_Serial)
            { Reset(); m_Owner = snapshot.owner; m_Serial = snapshot.ownerSerial; }
        }
        Entry& Find(const AmmoSnapshot& snapshot)
        {
            for (auto& entry : m_Entries)
                if (entry.weapon == snapshot.weapon && entry.serial == snapshot.weaponSerial) return entry;
            Entry& entry = m_Entries[m_Next++ % m_Entries.size()];
            entry = {}; entry.weapon = snapshot.weapon; entry.serial = snapshot.weaponSerial;
            return entry;
        }
        std::uintptr_t m_Owner = 0u;
        unsigned m_Serial = 0u, m_Next = 0u;
        std::array<Entry, 16> m_Entries{};
    };
}
