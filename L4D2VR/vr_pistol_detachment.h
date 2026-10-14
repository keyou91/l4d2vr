#pragma once
#include "vr_dual_pistols.h"
#include "vr_grip_release.h"

namespace l4d2vr_pistol
{
    using Hand = l4d2vr_dual::Hand;
    constexpr unsigned kRightDropMarker = 60u;
    constexpr unsigned kLeftDropMarker = 61u;
    inline unsigned Bit(Hand hand) { return hand == Hand::Right ? 1u : hand == Hand::Left ? 2u : 0u; }
    inline Hand DecodeDrop(unsigned word, bool attack)
    {
        if (attack) return Hand::None;
        return word == kRightDropMarker ? Hand::Right : word == kLeftDropMarker ? Hand::Left : Hand::None;
    }
    inline Hand DecodePickup(unsigned word, unsigned char impulse, bool use, bool attack)
    {
        if (impulse != l4d2vr_grip::kPickupImpulse || !use || attack) return Hand::None;
        return word == l4d2vr_dual::kRightShotMarker ? Hand::Right :
            word == l4d2vr_dual::kLeftShotMarker ? Hand::Left : Hand::None;
    }
    struct AmmoSplit { int retained = 0; int dropped = 0; };
    inline bool SplitAmmo(int clip, AmmoSplit& result)
    {
        if (clip < 0 || clip > 30) return false;
        // CPistol::RemoveDualWeapons halves its clip. Preserve the remainder
        // on the detached pistol instead of spawning a full magazine.
        result = { clip / 2, clip - clip / 2 };
        return result.retained <= 15 && result.dropped <= 15;
    }
    inline bool JoinAmmo(int held, int incoming, int& result)
    {
        if (held < 0 || held > 15 || incoming < 0 || incoming > 15) return false;
        result = held + incoming;
        return true;
    }

    // Native receive-state changes confirm a split. A release alone must not
    // hide an owned gun or invent a second entity on the client.
    class Ownership
    {
    public:
        void Observe(bool eligible, std::uintptr_t weapon, bool dual, std::uint32_t now)
        {
            if (!eligible || !weapon) { Reset(); return; }
            if (weapon != m_Weapon)
            {
                Reset(); m_Weapon = weapon; m_Dual = dual; m_Mask = dual ? 3u : 1u;
                return;
            }
            if (dual != m_Dual)
            {
                if (dual)
                {
                    m_Mask = 3u;
                    if (m_Pickup == Hand::Left) m_Left.AdoptWeapon(weapon);
                    if (m_Pickup == Hand::Right) m_Right.AdoptWeapon(weapon);
                }
                else
                {
                    m_Mask = m_LastDrop == Hand::Right ? 2u : 1u;
                    m_Queued &= m_Mask;
                    m_LastDrop = Hand::None;
                }
                m_Dual = dual; m_Awaiting = false; m_Pickup = Hand::None;
            }
            if (m_Awaiting && now - m_Started > 2000u)
            {
                // Do not retry a failed native transaction with a stale edge.
                m_Awaiting = false; m_Queued = 0u;
            }
        }
        void PickupRequested(Hand hand) { m_Pickup = hand; }
        void CancelPickup(Hand hand) { if (m_Pickup == hand) m_Pickup = Hand::None; }
        void AdoptPickup(Hand hand)
        {
            if (!m_Weapon || hand == Hand::None) return;
            if (!m_Dual) m_Mask = Bit(hand);
            (hand == Hand::Left ? m_Left : m_Right).AdoptWeapon(m_Weapon);
            m_Pickup = Hand::None;
        }
        Hand Release(bool rightActive, bool rightDown, bool leftActive, bool leftDown, std::uint32_t now,
            std::uint32_t rightDiscontinuity = 0u, std::uint32_t leftDiscontinuity = 0u)
        {
            if (rightDiscontinuity != m_RightDiscontinuity) m_Queued &= ~1u;
            if (leftDiscontinuity != m_LeftDiscontinuity) m_Queued &= ~2u;
            m_RightDiscontinuity = rightDiscontinuity;
            m_LeftDiscontinuity = leftDiscontinuity;
            if (m_Right.Update((m_Mask & 1u) != 0u, rightActive, rightDown, m_Weapon, rightDiscontinuity)) m_Queued |= 1u;
            if (m_Left.Update((m_Mask & 2u) != 0u, leftActive, leftDown, m_Weapon, leftDiscontinuity)) m_Queued |= 2u;
            if (!m_Weapon || m_Awaiting) return Hand::None;
            const unsigned available = m_Queued & m_Mask;
            const Hand hand = (available & 1u) ? Hand::Right : (available & 2u) ? Hand::Left : Hand::None;
            if (hand != Hand::None)
            {
                m_Queued &= ~Bit(hand); m_LastDrop = hand;
                m_Started = now; m_Awaiting = true;
            }
            return hand;
        }
        unsigned Mask() const { return m_Mask; }
        bool Awaiting() const { return m_Awaiting; }
        void CancelInteractions()
        {
            // Menus cancel input edges while keeping the actual inventory
            // hand. A split already sent may still be confirmed after a pause.
            m_Right.Reset(); m_Left.Reset(); m_Queued = 0u;
            m_Awaiting = false; m_Pickup = Hand::None;
        }
        void Reset()
        {
            m_Weapon = 0u; m_Mask = m_Queued = 0u; m_Dual = m_Awaiting = false;
            m_LastDrop = m_Pickup = Hand::None; m_Right.Reset(); m_Left.Reset();
        }
    private:
        std::uintptr_t m_Weapon = 0u;
        unsigned m_Mask = 0u, m_Queued = 0u;
        bool m_Dual = false, m_Awaiting = false;
        std::uint32_t m_Started = 0u;
        Hand m_LastDrop = Hand::None, m_Pickup = Hand::None;
        l4d2vr_grip::ReleaseLatch m_Right, m_Left;
        std::uint32_t m_RightDiscontinuity = 0u, m_LeftDiscontinuity = 0u;
    };
}
