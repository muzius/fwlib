#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
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

typedef struct {
    int lu;
    ODBST status;
} ResultatMachine;

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
                fprintf(stderr,
                        "CNC_%02d déclarée plusieurs fois (ligne %d)\n",
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

static const char *libelle_etat(short v)
{
    switch (v) {
        case 0: return "Reinit";
        case 1: return "Stop";
        case 2: return "Hold";
        case 3: return "Start";
        case 4: return "MSTR";
        default: return "Inconnu";
    }
}

static const char *libelle_alarme_court(short v)
{
    switch (v) {
        case 0: return "Non";
        case 1: return "CNC";
        case 8: return "PMC";
        default: return "Oui";
    }
}

static const char *libelle_mode_court(short v)
{
    switch (v) {
        case 0:  return "MDI";
        case 1:  return "Mem";
        case 3:  return "Edit";
        case 4:  return "Handle";
        case 5:  return "JOG";
        case 8:  return "INC";
        case 9:  return "REF";
        case 10: return "Remote";
        default: return "Inconnu";
    }
}

static const char *libelle_oui_non(short v)
{
    return v == 0 ? "Non" : "Oui";
}

static const char *libelle_urgence_court(short v)
{
    switch (v) {
        case 0: return "Non";
        case 1: return "Oui";
        case 2: return "Reinit";
        default: return "Inconnu";
    }
}

static void afficher_tableau(const Machine machines[],
                             const ResultatMachine resultats[],
                             int nombre_machines)
{
    puts("| Machine | État     | Alarme  | Mode     | Edit  | M/S/T/B | Urgence  | IP              | Heure    |");

    for (int i = 0; i < nombre_machines; ++i) {
        char heure[9];
        time_t maintenant = time(NULL);
        struct tm *instant = localtime(&maintenant);

        if (instant != NULL) {
            strftime(heure, sizeof(heure), "%H:%M:%S", instant);
        } else {
            strcpy(heure, "--:--:--");
        }

        if (!resultats[i].lu) {
            printf("| CNC %-3d | %-8s | %-7s | %-8s | %-5s | %-7s | %-8s | %-14.14s | %-8s |\n",
                   machines[i].numero,
                   "Erreur", "-", "-", "-", "-", "-",
                   machines[i].host, heure);
            continue;
        }

        const ODBST *s = &resultats[i].status;

        printf("| CNC %-3d | %d:%-6s | %d:%-5s | %d:%-6s | %d:%-3s | %-7d | %d:%-6s | %-14.14s | %-8s |\n",
               machines[i].numero,
               (int)s->run, libelle_etat(s->run),
               (int)s->alarm, libelle_alarme_court(s->alarm),
               (int)s->aut, libelle_mode_court(s->aut),
               (int)s->edit, libelle_oui_non(s->edit),
               (int)s->mstb,
               (int)s->emergency, libelle_urgence_court(s->emergency),
               machines[i].host,
               heure);
    }

    puts("---------------------------------------------------------------------------------------------------------------");
}

int main(void)
{
    Machine machines[MAX_MACHINES] = {0};
    ResultatMachine resultats[MAX_MACHINES] = {0};
    int ret = EXIT_SUCCESS;

    signal(SIGINT, gerer_signal_arret);
#ifndef _WIN32
    signal(SIGTERM, gerer_signal_arret);
#endif

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
        memset(resultats, 0, sizeof(resultats));

        for (int i = 0; i < nombre_machines && continuer; ++i) {
            if (!machines[i].handle_alloue) {
                continue;
            }

            resultats[i].status = (ODBST){0};

            int code = cnc_statinfo(machines[i].handle,
                                    &resultats[i].status);
            if (code == EW_OK) {
                resultats[i].lu = 1;
            } else {
                fprintf(stderr,
                        "Échec de lecture de CNC_%02d (code %d)\n",
                        machines[i].numero, code);
            }
        }

        afficher_tableau(machines, resultats, nombre_machines);
        fflush(stdout);

        if (continuer) {
            attendre_60_secondes();
        }
    }

    for (int i = 0; i < nombre_machines; ++i) {
        if (machines[i].handle_alloue) {
            int code = cnc_freelibhndl(machines[i].handle);
            if (code != EW_OK) {
                fprintf(stderr,
                        "Échec de libération du handle CNC_%02d (code %d)\n",
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