#ifndef BOOTSTRAP_H
#define BOOTSTRAP_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Arranque y diagnostico de plataforma (Practica 1, modularizado).
 * Visibilidad publica: solo la secuencia de POST + WDT.
 */
void bootstrap_init(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOTSTRAP_H */
