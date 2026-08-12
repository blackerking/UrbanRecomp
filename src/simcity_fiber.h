/* Fiber layer for the AOT frame boundary -- see src/simcity_fiber.c. */
#ifndef SIMCITY_FIBER_H
#define SIMCITY_FIBER_H

/* The game coroutine's body. Must never return; park in a yield loop instead
 * (the trampoline does this for you if it does). */
typedef void (*SimCityFiberEntry)(void);

/* Returns 1 on success. Idempotent. */
int  SimCityFiber_Create(SimCityFiberEntry entry);
void SimCityFiber_Destroy(void);
int  SimCityFiber_Created(void);

/* Host side: switch into the game coroutine and run until it yields. */
void SimCityFiber_RunOneFrame(void);

/* Game side: hand the frame back to the host. This is what the vblank HLE
 * (SimCity_WaitForVblank) calls once a host frame driver is installed. */
void SimCityFiber_YieldToHost(void);

extern unsigned long g_simcity_fiber_yields;

#endif /* SIMCITY_FIBER_H */
