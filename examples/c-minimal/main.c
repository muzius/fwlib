#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "../../fwlib32.h"

#define MACHINE_PORT 8193
#define NOMBRE_MACHINES 3

static volatile sig_atomic_t continuer = 1;

typedef struct {
    const char *numero;
    const char *host;
    unsigned short handle;
    int handle_alloue;
} Machine;

static void gerer_signal_arret(int signal_recu)
{
    (void)signal_recu;
    continuer = 0;
}

/* Vérifie le signal d’arrêt toutes les secondes. */
static void attendre_60_secondes(void)
{
    for (int i = 0; i < 60 && continuer; ++i) {
#ifdef _WIN32
        Sleep(1000);
#else
        struct timespec attente = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&attente, NULL);
#endif
    }
}

/* Libellés usuels FOCAS : à confirmer selon le modèle FANUC. */
static const char *libelle_mode(short v)
{
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

static const char *libelle_execution(short v)
{
    switch (v) {
        case 0: return "Réinitialisé";
        case 1: return "Arrêté (STOP)";
        case 2: return "Pause / maintien (HOLD)";
        case 3: return "En marche (START)";
        case 4: return "MSTR (état spécifique)";
        default: return "État inconnu ou dépendant du modèle";
    }
}

static const char *libelle_mouvement(short v)
{
    switch (v) {
        case 0: return "Aucun mouvement / autre";
        case 1: return "Mouvement d’axe";
        case 2: return "Temporisation (DWELL)";
        default: return "État inconnu ou dépendant du modèle";
    }
}

static const char *libelle_mstb(short v)
{
    switch (v) {
        case 0: return "Autre / inactif";
        case 1: return "Fin de fonction M/S/T/B (FIN)";
        default: return "État inconnu ou dépendant du modèle";
    }
}

static const char *libelle_urgence(short v)
{
    switch (v) {
        case 0: return "Pas d’arrêt d’urgence signalé";
        case 1: return "Arrêt d’urgence";
        case 2: return "Réinitialisation";
        default: return "État inconnu ou dépendant du modèle";
    }
}

static const char *libelle_alarme(short v)
{
    switch (v) {
        case 0: return "Aucune alarme / autre";
        case 1: return "Alarme CNC";
        case 8: return "Alarme PMC";
        default: return "Alarme ou état à vérifier";
    }
}

static const char *libelle_edition(short v)
{
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

static void afficher_etat(const char *nom, short code, const char *texte)
{
    printf("%-12s : %s (code %d)\n", nom, texte, (int)code);
}

static void interroger_machine(const Machine *machine)
{
    ODBST status = {0};

    printf("\n=== Machine %s — %s ===\n", machine->numero, machine->host);

    int code = cnc_statinfo(machine->handle, &status);
    if (code != EW_OK) {
        fprintf(stderr,
                "Échec de lecture de l’état de la machine %s (code %d)\n",
                machine->numero, code);
        return;
    }

    afficher_etat("Mode", status.aut, libelle_mode(status.aut));
    afficher_etat("Exécution", status.run, libelle_execution(status.run));
    afficher_etat("Mouvement", status.motion, libelle_mouvement(status.motion));
    afficher_etat("M/S/T/B", status.mstb, libelle_mstb(status.mstb));
    afficher_etat("Urgence", status.emergency, libelle_urgence(status.emergency));
    afficher_etat("Alarme", status.alarm, libelle_alarme(status.alarm));
    afficher_etat("Édition", status.edit, libelle_edition(status.edit));
}

int main(void)
{
    int ret = EXIT_SUCCESS;

    Machine machines[NOMBRE_MACHINES] = {
        { "01", getenv("MACHINE_01"), 0, 0 },
        { "02", getenv("MACHINE_02"), 0, 0 },
        { "03", getenv("MACHINE_03"), 0, 0 }
    };

    /* Portainer envoie normalement SIGTERM à l’arrêt du conteneur. */
    signal(SIGINT, gerer_signal_arret);
#ifndef _WIN32
    signal(SIGTERM, gerer_signal_arret);
#endif

#ifndef _WIN32
    if (cnc_startupprocess(0, "focas.log") != EW_OK) {
        fprintf(stderr, "Impossible de créer focas.log\n");
        return EXIT_FAILURE;
    }
#endif

    /* Connexion à chaque machine configurée. */
    for (int i = 0; i < NOMBRE_MACHINES; ++i) {
        if (machines[i].host == NULL || machines[i].host[0] == '\0') {
            fprintf(stderr, "Adresse manquante : variable MACHINE_%s non définie.\n",
                    machines[i].numero);
            continue;
        }

        printf("Connexion à la machine %s (%s:%d)...\n",
               machines[i].numero, machines[i].host, MACHINE_PORT);
        fflush(stdout);

        int code = cnc_allclibhndl3(machines[i].host, MACHINE_PORT, 10,
                                    &machines[i].handle);
        if (code != EW_OK) {
            fprintf(stderr,
                    "Échec de connexion à la machine %s (code %d)\n",
                    machines[i].numero, code);
            ret = EXIT_FAILURE;
            continue;
        }

        machines[i].handle_alloue = 1;
    }

    while (continuer) {
        for (int i = 0; i < NOMBRE_MACHINES && continuer; ++i) {
            if (machines[i].handle_alloue) {
                interroger_machine(&machines[i]);
            }
        }

        fflush(stdout);

        if (continuer) {
            attendre_60_secondes();
        }
    }

    /* Libération des handles alloués. */
    for (int i = 0; i < NOMBRE_MACHINES; ++i) {
        if (machines[i].handle_alloue) {
            int code = cnc_freelibhndl(machines[i].handle);
            if (code != EW_OK) {
                fprintf(stderr,
                        "Échec de libération du handle de la machine %s "
                        "(code %d)\n",
                        machines[i].numero, code);
                ret = EXIT_FAILURE;
            }
        }
    }

#ifndef _WIN32
    cnc_exitprocess();
#endif

    return ret;
}