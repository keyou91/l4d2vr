#include "../L4D2VR/vr_pistol_prediction.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Pistol prediction failed at line %d\n", __LINE__); std::abort(); } } while(false)

int main()
{
    using namespace l4d2vr_pistol_prediction;
    using l4d2vr_pistol_sync::State;
    CHECK(FiringMask(Pack(15, 15, 1u, 2u)) == 2u); // right magazine inserted, chamber still empty
    CHECK(FiringMask(Pack(15, 15, 1u, 3u)) == 3u);
    CHECK(FiringMask(Pack(15, 15, 2u, 1u)) == 1u);
    CHECK(FiringMask(Pack(-1, -1, 0u, 0u)) == 3u);
    Journal journal;
    State state{42u, 1u, 0x3005u, 100u, 15, 16, 1, 15, true, true};
    int right = -1, left = -1;
    CHECK(journal.Refresh(10u, state));
    CHECK(journal.Counts(16, 0u, 0u, right, left) && right == 1 && left == 15);
    CHECK(journal.Record(101u, 1u, Hand::Right, 16, 15, 1u));
    CHECK(journal.Counts(15, 0u, 0u, right, left) && right == 0 && left == 15);
    CHECK(BlocksEmptyHand(true, true, Hand::Right, right, left));
    CHECK(!BlocksEmptyHand(true, true, Hand::Left, right, left));
    CHECK(!BlocksEmptyHand(true, false, Hand::Right, right, left));
    CHECK(!BlocksEmptyHand(false, true, Hand::Right, right, left));
    CHECK(!BlocksEmptyHand(true, true, Hand::Right, -1, -1));
    CHECK(!BlocksEmptyHand(true, true, Hand::None, 0, 0));
    CHECK(journal.Record(101u, 1u, Hand::Right, 16, 15, 1u)); // same prediction replay
    CHECK(journal.Record(102u, 1u, Hand::Left, 15, 14, 1u));
    CHECK(journal.Counts(14, 0u, 0u, right, left) && right == 0 && left == 14);
    CHECK(journal.Counts(16, 101u, 1u, right, left) && right == 1 && left == 15);
    CHECK(journal.Counts(15, 101u, 2u, right, left) && right == 0 && left == 15);
    CHECK(journal.Record(101u, 1u, Hand::Right, 16, 15, 1u)); // newer command cannot consume this replay's round
    CHECK(journal.Counts(14, 0u, 0u, right, left) && right == 0 && left == 14);
    CHECK(!journal.Counts(13, 0u, 0u, right, left)); // unexplained native delta stays unknown
    CHECK(!journal.Record(103u, 1u, Hand::Right, 14, 13, 1u)); // native wrong-hand consumption
    CHECK(!journal.Refresh(10u, state)); // same baseline cannot repair attribution
    state.sequence = 2u; state.command = 102u; state.clip = 14; state.right = 0; state.left = 14;
    CHECK(journal.Refresh(10u, state) && journal.Counts(14, 0u, 0u, right, left));
    CHECK(journal.Record(103u, 1u, Hand::Left, 14, 13, 1u));
    CHECK(!journal.Record(104u, 1u, Hand::Left, 13, 12, 2u)); // bullets and clip consumption disagree
    state.sequence = 3u; state.command = 103u; state.clip = 13; state.left = 13;
    CHECK(journal.Refresh(10u, state));
    CHECK(!journal.Counts(30, 0u, 0u, right, left)); // native reload needs a new authoritative baseline
    state.sequence = 4u; state.command = 110u; state.clip = 30; state.right = state.left = 15;
    CHECK(journal.Refresh(10u, state));
    CHECK(journal.Record(111u, 1u, Hand::Left, 30, 28, 2u));
    CHECK(journal.Record(111u, 2u, Hand::Right, 28, 27, 1u));
    CHECK(journal.Counts(27, 0u, 0u, right, left) && right == 14 && left == 13);
    CHECK(journal.Counts(30, 111u, 1u, right, left) && right == 15 && left == 15);
    CHECK(journal.Counts(28, 111u, 2u, right, left) && right == 15 && left == 13);
    CHECK(journal.Record(111u, 1u, Hand::Left, 30, 28, 2u));
    state.sequence = 5u; state.command = 111u; state.clip = 27; state.right = 14; state.left = 13;
    CHECK(journal.Refresh(10u, state));
    CHECK(!journal.Counts(30, 111u, 1u, right, left)); // cannot predict behind the server acknowledgement
    CHECK(journal.Record(111u, 1u, Hand::Left, 30, 28, 2u)); // already acknowledged, never charged again
    CHECK(journal.Counts(27, 0u, 0u, right, left) && right == 14 && left == 13);
    CHECK(journal.Record(112u, 1u, Hand::Left, 27, 26, 1u));
    CHECK(journal.Record(112u, 1u, Hand::Left, 27, 27, 0u)); // replay no longer fires
    CHECK(journal.Counts(27, 0u, 0u, right, left) && right == 14 && left == 13);
    CHECK(journal.Record(112u, 1u, Hand::Left, 27, 27, 2u)); // native mode emits bullets without clip consumption
    CHECK(journal.Counts(27, 0u, 0u, right, left));
    auto stale = state; --stale.sequence; CHECK(!journal.Refresh(10u, stale));
    stale = state; ++stale.sequence; --stale.command; CHECK(!journal.Refresh(10u, stale));
    stale = state; --stale.right; --stale.clip; CHECK(!journal.Refresh(10u, stale)); // immutable sequence
    CHECK(!journal.Record(112u, 1u, Hand::None, 27, 26, 1u));
    CHECK(!journal.Record(0u, 1u, Hand::Left, 27, 26, 1u));
    CHECK(!journal.Record(112u, 65u, Hand::Left, 27, 26, 1u));
    CHECK(!journal.Record(112u, 1u, Hand::Left, (std::numeric_limits<int>::max)(), -1, 1u));
    state.token = 99u; state.sequence = 1u; state.command = 1u;
    CHECK(journal.Refresh(20u, state) && journal.Counts(27, 0u, 0u, right, left));
    state.handle = 0x4005u; state.capacity = 8; state.clip = 16; state.right = state.left = 8;
    CHECK(journal.Refresh(20u, state));
    CHECK(journal.Record(2u, 1u, Hand::Left, 16, 15, 1u));
    CHECK(journal.Counts(15, 0u, 0u, right, left) && right == 8 && left == 7);
    auto unknown = state; unknown.known = false; unknown.right = unknown.left = 0;
    CHECK(!journal.Refresh(20u, unknown) && !journal.Counts(15, 0u, 0u, right, left));
    CHECK(journal.Refresh(20u, state) && !journal.Counts(15, 0u, 0u, right, left)); // missing history cannot invent a hand
    CHECK(!journal.Refresh(0u, state));
    journal.Reset(); CHECK(!journal.Counts(16, 0u, 0u, right, left));

    // A host acknowledgement removes only settled events; later prediction is
    // still projected on top of the updated partition.
    state = {42u, 1u, 0x3005u, 100u, 15, 30, 15, 15, true, true};
    CHECK(journal.Refresh(10u, state));
    CHECK(journal.Record(101u, 1u, Hand::Right, 30, 29, 1u));
    CHECK(journal.Record(102u, 1u, Hand::Left, 29, 28, 1u));
    state.sequence = 2u; state.command = 101u; state.clip = 29; state.right = 14;
    CHECK(journal.Refresh(10u, state));
    CHECK(journal.Counts(28, 0u, 0u, right, left) && right == 14 && left == 14);
    CHECK(!journal.Record(102u, 1u, Hand::Right, 29, 28, 1u)); // immutable hand decision
    CHECK(!journal.Counts(28, 0u, 0u, right, left));
    std::puts("Per-hand native prediction and empty-hand gating checks passed");
}
