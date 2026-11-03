// FlatIdMap against std::unordered_map under random insert/erase churn,
// which is what exercises backward-shift deletion.

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

#include "lob/flat_id_map.hpp"

using lob::FlatIdMap;
using lob::OrderId;

TEST(FlatIdMap, BasicInsertFindErase) {
  FlatIdMap<int*> m(8);
  int a = 1, b = 2;
  EXPECT_EQ(m.find(5), nullptr);
  EXPECT_TRUE(m.insert(5, &a));
  EXPECT_TRUE(m.insert(6, &b));
  EXPECT_EQ(m.find(5), &a);
  EXPECT_EQ(m.find(6), &b);
  EXPECT_TRUE(m.erase(5));
  EXPECT_FALSE(m.erase(5));
  EXPECT_EQ(m.find(5), nullptr);
  EXPECT_EQ(m.find(6), &b);
  EXPECT_EQ(m.size(), 1u);
}

TEST(FlatIdMap, RefusesInsertPastHalfLoad) {
  FlatIdMap<int*> m(8);  // capacity 16, at most 8 keys
  int x = 0;
  for (OrderId k = 1; k <= m.capacity() / 2; ++k) EXPECT_TRUE(m.insert(k, &x));
  EXPECT_FALSE(m.insert(999, &x));
}

TEST(FlatIdMap, MatchesUnorderedMapUnderChurn) {
  constexpr std::size_t kMax = 512;
  FlatIdMap<std::uintptr_t> m(kMax);
  std::unordered_map<OrderId, std::uintptr_t> ref;
  std::vector<OrderId> live;
  std::mt19937_64 rng(7);

  for (int step = 0; step < 400'000; ++step) {
    const bool do_insert = live.empty() || (live.size() < kMax && rng() % 2);
    if (do_insert) {
      // small key space so keys collide and get reused a lot
      OrderId k = 1 + rng() % 4096;
      if (ref.count(k)) continue;
      const std::uintptr_t v = rng() | 1;
      ASSERT_TRUE(m.insert(k, v));
      ref[k] = v;
      live.push_back(k);
    } else {
      const std::size_t i = rng() % live.size();
      const OrderId k = live[i];
      ASSERT_TRUE(m.erase(k));
      ref.erase(k);
      live[i] = live.back();
      live.pop_back();
    }
    if (step % 97 == 0) {
      ASSERT_EQ(m.size(), ref.size());
      for (OrderId k : live) ASSERT_EQ(m.find(k), ref[k]) << "step " << step;
      ASSERT_EQ(m.find(5000), 0u);  // never inserted
    }
  }
}
