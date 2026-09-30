#ifndef ENIL_ATOMIC_H
#define ENIL_ATOMIC_H

/* GCC/Clang legacy atomics are available on Tiger PPC as well as ARM/x86.
 * Use these for EVERY access to a flag shared across threads. Volatile alone
 * does not provide synchronization. CAS supplies a full memory barrier. */
static inline int enil_atomic_load(const volatile int *value) {
  return __sync_val_compare_and_swap((volatile int *)value, 0, 0);
}
static inline int enil_atomic_exchange(volatile int *value, int next) {
  int previous;
  do { previous = enil_atomic_load(value); }
  while (!__sync_bool_compare_and_swap(value, previous, next));
  return previous;
}
static inline void enil_atomic_store(volatile int *value, int next) {
  (void)enil_atomic_exchange(value, next);
}
#endif
