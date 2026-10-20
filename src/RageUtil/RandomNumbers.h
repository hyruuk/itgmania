#ifndef RAGEUTIL_RANDOMNUMBERS_H
#define RAGEUTIL_RANDOMNUMBERS_H

#include <ctime>
#include <random>

/* The seed a generator gets when it is given 0: the time of day, or in gym
 * mode a constant, so that a gym run never depends on when it started. */
unsigned int DefaultRandomSeed();

class MersenneTwister : public std::mt19937 {
 public:
  MersenneTwister(int iSeed = 0)
      : std::mt19937(iSeed == 0 ? DefaultRandomSeed() : iSeed) {}
};

typedef MersenneTwister RandomGen;

extern RandomGen g_RandomNumberGenerator;

/* Reseed g_RandomNumberGenerator and the generator behind Lua's math.random. */
void SeedRandomNumberGenerators(unsigned int iSeed);

/**
 * @brief Return a float between the low and high values.
 * @param fLow the low value, inclusive.
 * @param fHigh the high value, inclusive.
 * @return the random float.
 */
inline float RandomFloat(float fLow, float fHigh) {
  std::uniform_real_distribution<> dist(fLow, fHigh);
  return dist(g_RandomNumberGenerator);
}

/**
 * @brief Generate a random float between 0 inclusive and 1 exclusive.
 * @return the random float.
 */
inline float RandomFloat() { return RandomFloat(0, 1); }

// Returns an integer between nLow and nHigh inclusive
inline int RandomInt(int nLow, int nHigh) {
  std::uniform_int_distribution<> dist(nLow, nHigh);
  return dist(g_RandomNumberGenerator);
}

// Returns an integer between 0 and n-1 inclusive (replacement for rand() % n).
inline int RandomInt(int n) { return RandomInt(0, n - 1); }

// Simple function for generating random numbers
inline float randomf(const float low = -1.0f, const float high = 1.0f) {
  return RandomFloat(low, high);
}

#endif