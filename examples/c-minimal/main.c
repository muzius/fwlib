#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "../../fwlib32.h"

#define MACHINE_PORT 8193
#define MAX_MACHINES 100
#define CONFIG_FILE "/config/liste-cnc.conf"
#define MAX_LINE_LENGTH 256
#define MAX_HOST_LENGTH 64

static volatile sig_atomic_t continuer = 1;

typedef struct {
    int numero;
    char host[MAX_HOST_LENGTH];
    unsigned short handle;
    int handle_alloue;
} Machine;

static void gerer_signal_arret(int signal_recu)
{
    (void)signal_recu;
    continuer = 0;
}

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

/* Lit une ligne CNC_NN: "adresse IP" ; ignore les lignes vides et commentaires. */
static int charger_configuration(Machine machines[], int capacite)
{
    FILE *fichier = fopen(CONFIG_FILE, "r");
    if (fichier == NULL) {
        fprintf(stderr, "Impossible d’ouvrir %s : %s\n",
                CONFIG_FILE, strerror(errno));
        return -1;
    }

    char ligne[MAX_LINE_LENGTH];
    int nombre = 0;
    int numero_ligne = 0;

    while (fgets(ligne, sizeof(ligne), fichier) != NULL) {
        ++numero_ligne;

        char *p = ligne;
        while (isspace((unsigned char)*p)) {
            ++p;
        }

        if (*p == '\0' || *p == '#' || *p == '\n') {
            continue;
        }

        int numero;
        int position = 0;

        if (sscanf(p, " CNC_%d : %n", &numero, &position) != 1 ||
            position == 0 || numero < 1 || numero > 999) {
            fprintf(stderr, "Format invalide dans %s, ligne %d\n",
                    CONFIG_FILE, numero_ligne);
            fclose(fichier);
            return -1;
        }

        p += position;
        while (isspace((unsigned char)*p)) {
            ++p;
        }

        char host[MAX_HOST_LENGTH];
        size_t longueur = 0;

        if (*p == '"') {
            ++p;
            char *fin = strchr(p, '"');
            if (fin == NULL) {
                fprintf(stderr, "Adresse sans guillemet fermant, ligne %d\n",
                        numero_ligne);
                fclose(fichier);
                return -1;
            }
            longueur = (size_t)(fin - p);
            if (longueur == 0 || longueur >= sizeof(host)) {
                fprintf(stderr, "Adresse invalide, ligne %d\n", numero_ligne);
                fclose(fichier);
                return -1;
            }
            memcpy(host, p, longueur);
            host[longueur] = '\0';
        } else {
            char *fin = p;
            while (*fin != '\0' && *fin != '\n' && *fin != '\r' &&
                   *fin != '#' && !isspace((unsigned char)*fin)) {
                ++fin;
            }
            longueur = (size_t)(fin - p);
            if (longueur == 0 || longueur >= sizeof(host)) {
                fprintf(stderr, "Adresse invalide, ligne %d\n", numero_ligne);
                fclose(fichier);
                return -1;
            }
            memcpy(host, p, longueur);
            host[longueur] = '\0';
        }

        if (nombre >= capacite) {
            fprintf(stderr, "Trop de machines : maximum %d\n", capacite);
            fclose(fichier);
            return -1;
        }

        for (int i = 0; i < nombre; ++i) {
            if (machines[i].numero == numero) {
                fprintf(stderr, "CNC_%02d déclarée plusieurs fois (ligne %d)\n",
                        numero, numero_ligne);
                fclose(fichier);
                return -1;
            }
        }

        machines[nombre].numero = numero;
        strcpy(machines[nombre].host, host);
        ++nombre;
    }

    if (ferror(fichier)) {
        fprintf(stderr, "Erreur de lecture de %s\n", CONFIG_FILE);
        fclose(fichier);
        return -1;
    }

    fclose(fichier);

    if (nombre == 0) {
        fprintf(stderr, "Aucune CNC définie dans %s\n", CONFIG_FILE);
        return -1;
    }

    return nombre;
}

/* Libellés usuels FOCAS, à confirmer selon le modèle FANUC. */
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

    printf("\n=== Machine CNC_%02d — %s ===\n",
           machine->numero, machine->host);

    int code = cnc_statinfo(machine->handle, &status);
    if (code != EW_OK) {
        fprintf(stderr,
                "Échec de lecture de CNC_%02d (code %d)\n",
                machine->numero, code);
        return;
    }

    afficher_etat("Mode", status.aut, libelle_mode(status.aut));
    afficher_etat("Exécution", status.run, libelle_execution(status.run));
    afficher_etat("Mouvement", status.motion, libelle_mouvement(status.motion));
    afficher_etat("M/S/T/B", status.mstb, libelle_mstb(status.mstb));
    afficher_etat("Urgence", status.emergency,
                  libelle_urgence(status.emergency));
    afficher_etat("Alarme", status.alarm, libelle_alarme(status.alarm));
    afficher_etat("Édition", status.edit, libelle_edition(status.edit));
}

int main(void)
{
    Machine machines[MAX_MACHINES] = {0};
    int ret = EXIT_SUCCESS;

    signal(SIGINT, gerer_signal_arret);
#ifndef _WIN32
    signal(SIGTERM, gerer_signal_arret);
#endif

    /* La configuration est vérifiée avant toute initialisation FOCAS. */
    int nombre_machines = charger_configuration(machines, MAX_MACHINES);
    if (nombre_machines < 0) {
        return EXIT_FAILURE;
    }

#ifndef _WIN32
    if (cnc_startupprocess(0, "focas.log") != EW_OK) {
        fprintf(stderr, "Impossible de créer focas.log\n");
        return EXIT_FAILURE;
    }
#endif

    for (int i = 0; i < nombre_machines; ++i) {
        printf("Connexion à CNC_%02d (%s:%d)...\n",
               machines[i].numero, machines[i].host, MACHINE_PORT);
        fflush(stdout);

        int code = cnc_allclibhndl3(machines[i].host, MACHINE_PORT, 10,
                                    &machines[i].handle);
        if (code != EW_OK) {
            fprintf(stderr, "Échec de connexion à CNC_%02d (code %d)\n",
                    machines[i].numero, code);
            ret = EXIT_FAILURE;
            continue;
        }

        machines[i].handle_alloue = 1;
    }

    while (continuer) {
        for (int i = 0; i < nombre_machines && continuer; ++i) {
            if (machines[i].handle_alloue) {
                interroger_machine(&machines[i]);
            }
        }

        fflush(stdout);

        if (continuer) {
            attendre_60_secondes();
        }
    }

    for (int i = 0; i < nombre_machines; ++i) {
        if (machines[i].handle_alloue) {
            int code = cnc_freelibhndl(machines[i].handle);
            if (code != EW_OK) {
                fprintf(stderr, "Échec de libération du handle CNC_%02d "
                        "(code %d)\n", machines[i].numero, code);
                ret = EXIT_FAILURE;
            }
        }
    }

#ifndef _WIN32
    cnc_exitprocess();
#endif

    return ret;
}