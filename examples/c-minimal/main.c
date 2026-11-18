#include <stdio.h>
#include <stdlib.h>

#include "../../fwlib32.h"

#define DEFAULT_MACHINE_HOST "127.0.0.1"
#define MACHINE_PORT 8193

int main(void) {
  int allocated = 0;
  int ret = 0;
  unsigned short libh;
  ODBST status = {0};

  const char *machine_host = getenv("MACHINE_HOST");
  if (machine_host == NULL) {
    machine_host = DEFAULT_MACHINE_HOST;
  }

#ifndef _WIN32
  if (cnc_startupprocess(0, "focas.log") != EW_OK) {
    fprintf(stderr, "Impossible de créer focas.log\n");
    return EXIT_FAILURE;
  }
#endif

  printf("Connexion à %s:%d...\n", machine_host, MACHINE_PORT);

  ret = cnc_allclibhndl3(machine_host, MACHINE_PORT, 10, &libh);
  if (ret != EW_OK) {
    fprintf(stderr, "Connexion à la CNC impossible (code %d)\n", ret);
    goto cleanup;
  }
  allocated = 1;

  ret = cnc_statinfo(libh, &status);
  if (ret != EW_OK) {
    fprintf(stderr, "Lecture de l’état CNC impossible (code %d)\n", ret);
    goto cleanup;
  }

  printf("Mode T/M : %d\n", status.tmmode);
  printf("Mode automatique : %d\n", status.aut);
  printf("État d’exécution : %d\n", status.run);
  printf("Mouvement / temporisation : %d\n", status.motion);
  printf("État M/S/T/B : %d\n", status.mstb);
  printf("Arrêt d’urgence : %d\n", status.emergency);
  printf("Alarme : %d\n", status.alarm);
  printf("Édition : %d\n", status.edit);

  ret = EXIT_SUCCESS;

cleanup:
  if (allocated && cnc_freelibhndl(libh) != EW_OK) {
    fprintf(stderr, "Échec de libération du handle FOCAS\n");
  }
#ifndef _WIN32
  cnc_exitprocess();
#endif

  return ret == EXIT_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
}