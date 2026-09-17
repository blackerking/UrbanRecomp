/* Fiber layer for the AOT frame boundary -- see src/sc_fiber.c. */
#ifndef SC_FIBER_H_INCLUDED
#define SC_FIBER_H_INCLUDED

/* The game coroutine's body. Must never return; park in a yield loop instead
 * (the trampoline does this for you if it does). */
typedef void (*ScFiberEntry)(void);

/* Returns 1 on success. Idempotent. */
int  ScFiber_Create(ScFiberEntry entry);
void ScFiber_Destroy(void);
int  ScFiber_Created(void);

/* Host side: switch into the game coroutine and run until it yields. */
void ScFiber_RunOneFrame(void);

/* Game side: hand the frame back to the host. This is what the vblank HLE
 * (ScHle_WaitForVblank) calls once a host frame driver is installed. */
void ScFiber_YieldToHost(void);

extern unsigned long g_sc_fiber_yields;

#endif /* SC_FIBER_H_INCLUDED */
