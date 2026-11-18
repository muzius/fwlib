#include <stdio.h>
#include <stdlib.h>

#include "../../fwlib32.h"

#define MACHINE_PORT 8193

/* Libellés usuels FOCAS : à confirmer selon le modèle FANUC. */
static const char *libelle_mode(short v) {
  switch (v) {
    case 0:  return "MDI";
    case 1:  return "Mémoire (MEM)";
    case 3:  return "Édition (EDIT)";
    case 4:  return "Manivelle (HANDLE)";
    case 5:  return "JOG";
    case 8:  return "Incrémental (INC)";
    case 9:  return "Retour référence (REF)";
    case 10: return "À distance (REMOTE)";
    default: return "Mode inconnu ou dépendant du modèle";
  }
}

static const char *libelle_execution(short v) {
  switch (v) {
    case 0: return "Réinitialisé";
    case 1: return "Arrêté (STOP)";
    case 2: return "Pause / maintien (HOLD)";
    case 3: return "En marche (START)";
    case 4: return "MSTR (état spécifique)";
    default: return "État inconnu ou dépendant du modèle";
  }
}

static const char *libelle_mouvement(short v) {
  switch (v) {
    case 0: return "Aucun mouvement / autre";
    case 1: return "Mouvement d’axe";
    case 2: return "Temporisation (DWELL)";
    default: return "État inconnu ou dépendant du modèle";
  }
}

static const char *libelle_mstb(short v) {
  switch (v) {
    case 0: return "Autre / inactif";
    case 1: return "Fin de fonction M/S/T/B (FIN)";
    default: return "État inconnu ou dépendant du modèle";
  }
}

static const char *libelle_urgence(short v) {
  switch (v) {
    case 0: return "Pas d’arrêt d’urgence signalé";
    case 1: return "Arrêt d’urgence";
    case 2: return "Réinitialisation";
    default: return "État inconnu ou dépendant du modèle";
  }
}

static const char *libelle_alarme(short v) {
  switch (v) {
    case 0: return "Aucune alarme / autre";
    case 1: return "Alarme CNC";
    case 8: return "Alarme PMC";
    default: return "Alarme ou état à vérifier";
  }
}

static const char *libelle_edition(short v) {
  switch (v) {
    case 0: return "Pas d’édition";
    case 1: return "Édition de programme";
    case 2: return "Recherche";
    case 3: return "Sortie / transfert";
    case 4: return "Entrée / transfert";
    case 5: return "Comparaison";
    default: return "Autre état d’édition";
  }
}

static void afficher_etat(const char *nom, short code, const char *texte) {
  printf("%-12s : %s (code %d)\n", nom, texte, (int)code);
}

int main(void) {
  int allocated = 0;
  int ret = 0;
  unsigned short libh;

  const char *machine_host = getenv("MACHINE_HOST");
  if (machine_host == NULL || machine_host[0] == '\0') {
    fprintf(stderr, "Adresse IP manquante : définissez la variable MACHINE_HOST.\n");
    return EXIT_FAILURE;
  }

#ifndef _WIN32
  if (cnc_startupprocess(0, "focas.log") != EW_OK) {
    fprintf(stderr, "Impossible de créer focas.log\n");
    return EXIT_FAILURE;
  }
#endif

  printf("Connexion à la machine %s:%d...\n", machine_host, MACHINE_PORT);

  ret = cnc_allclibhndl3(machine_host, MACHINE_PORT, 10, &libh);
  if (ret != EW_OK) {
    fprintf(stderr, "Échec de connexion à la CNC (code %d)\n", ret);
    ret = EXIT_FAILURE;
    goto cleanup;
  }
  allocated = 1;

  {
    ODBST status = {0};

    ret = cnc_statinfo(libh, &status);
    if (ret != EW_OK) {
      fprintf(stderr, "Échec de lecture de l’état CNC (code %d)\n", ret);
      ret = EXIT_FAILURE;
      goto cleanup;
    }

    printf("\nÉtat de la CNC :\n");
    afficher_etat("Mode", status.aut, libelle_mode(status.aut));
    afficher_etat("Exécution", status.run,
                  libelle_execution(status.run));
    afficher_etat("Mouvement", status.motion,
                  libelle_mouvement(status.motion));
    afficher_etat("M/S/T/B", status.mstb, libelle_mstb(status.mstb));
    afficher_etat("Urgence", status.emergency,
                  libelle_urgence(status.emergency));
    afficher_etat("Alarme", status.alarm, libelle_alarme(status.alarm));
    afficher_etat("Édition", status.edit, libelle_edition(status.edit));
  }

  ret = EXIT_SUCCESS;

cleanup:
  if (allocated && cnc_freelibhndl(libh) != EW_OK) {
    fprintf(stderr, "Échec de libération du handle FOCAS\n");
    if (ret == EXIT_SUCCESS) {
      ret = EXIT_FAILURE;
    }
  }

#ifndef _WIN32
  cnc_exitprocess();
#endif

  return ret;
}