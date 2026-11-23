#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <mosquitto.h>
#include "fwlib32.h"

#define MACHINE_PORT 8193
#define MAX_MACHINES 100
#define CONFIG_CNC_FILE "/config/liste-cnc.conf"
#define CONFIG_MQTT_FILE "/config/mqtt.conf"
#define MAX_LINE_LENGTH 256
#define MAX_HOST_LENGTH 64
#define MAX_MQTT_BROKERS 3

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
    long part_count;        /* Compteur de pièces (paramètre #6711) */
    long cycle_time_ms;     /* Temps de cycle en millisecondes (paramètre #6758 ou #6757) */
} ResultatMachine;

typedef struct {
    int actif;
    char host[MAX_HOST_LENGTH];
    int port;
    char username[64];
    char password[64];
    char topic_prefix[128];
    struct mosquitto *mosq;
    int connecte;
} MqttBroker;

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

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

/* Lecture du fichier liste-cnc.conf */
static int charger_configuration_cnc(Machine machines[], int capacite)
{
    FILE *fichier = fopen(CONFIG_CNC_FILE, "r");
    if (fichier == NULL) {
        fprintf(stderr, "Impossible d'ouvrir %s : %s\n", CONFIG_CNC_FILE, strerror(errno));
        return -1;
    }

    char ligne[MAX_LINE_LENGTH];
    int nombre = 0;
    int numero_ligne = 0;

    while (fgets(ligne, sizeof(ligne), fichier) != NULL) {
        ++numero_ligne;
        char *p = ligne;
        while (isspace((unsigned char)*p)) p++;

        if (*p == '\0' || *p == '#' || *p == '\n') continue;

        int numero;
        int position = 0;
        if (sscanf(p, " CNC_%d : %n", &numero, &position) != 1 || position == 0 || numero < 1 || numero > 999) {
            fprintf(stderr, "Format invalide dans %s, ligne %d\n", CONFIG_CNC_FILE, numero_ligne);
            fclose(fichier);
            return -1;
        }

        p += position;
        while (isspace((unsigned char)*p)) p++;

        char host[MAX_HOST_LENGTH];
        size_t longueur = 0;

        if (*p == '"') {
            p++;
            char *fin = strchr(p, '"');
            if (!fin) {
                fprintf(stderr, "Adresse sans guillemet fermant, ligne %d\n", numero_ligne);
                fclose(fichier);
                return -1;
            }
            longueur = (size_t)(fin - p);
            if (longueur == 0 || longueur >= sizeof(host)) {
                fclose(fichier);
                return -1;
            }
            memcpy(host, p, longueur);
            host[longueur] = '\0';
        } else {
            char *fin = p;
            while (*fin && *fin != '\n' && *fin != '\r' && *fin != '#' && !isspace((unsigned char)*fin)) {
                fin++;
            }
            longueur = (size_t)(fin - p);
            if (longueur == 0 || longueur >= sizeof(host)) {
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

        machines[nombre].numero = numero;
        strcpy(machines[nombre].host, host);
        nombre++;
    }

    fclose(fichier);
    return nombre;
}

/* Lecture du fichier mqtt.conf */
static int charger_configuration_mqtt(MqttBroker brokers[], int max_brokers)
{
    FILE *fichier = fopen(CONFIG_MQTT_FILE, "r");
    if (fichier == NULL) {
        fprintf(stderr, "Impossible d'ouvrir %s : %s\n", CONFIG_MQTT_FILE, strerror(errno));
        return -1;
    }

    char ligne[MAX_LINE_LENGTH];
    int index_courant = -1;

    for (int i = 0; i < max_brokers; ++i) {
        memset(&brokers[i], 0, sizeof(MqttBroker));
        brokers[i].port = 1883;
        strcpy(brokers[i].topic_prefix, "juradec/cnc");
    }

    while (fgets(ligne, sizeof(ligne), fichier) != NULL) {
        char *p = trim(ligne);
        if (*p == '\0' || *p == '#') continue;

        if (*p == '[' && strchr(p, ']')) {
            int broker_num = 0;
            if (sscanf(p, "[MQTT_%d]", &broker_num) == 1 && broker_num >= 1 && broker_num <= max_brokers) {
                index_courant = broker_num - 1;
                brokers[index_courant].actif = 1;
            } else {
                index_courant = -1;
            }
            continue;
        }

        if (index_courant < 0) continue;

        char cle[64], valeur[128];
        if (sscanf(p, "%63[^=]=%127[^\n]", cle, valeur) == 2) {
            char *c = trim(cle);
            char *v = trim(valeur);

            if (strcmp(c, "host") == 0) {
                strncpy(brokers[index_courant].host, v, sizeof(brokers[index_courant].host) - 1);
            } else if (strcmp(c, "port") == 0) {
                brokers[index_courant].port = atoi(v);
            } else if (strcmp(c, "username") == 0) {
                strncpy(brokers[index_courant].username, v, sizeof(brokers[index_courant].username) - 1);
            } else if (strcmp(c, "password") == 0) {
                strncpy(brokers[index_courant].password, v, sizeof(brokers[index_courant].password) - 1);
            } else if (strcmp(c, "topic_prefix") == 0) {
                strncpy(brokers[index_courant].topic_prefix, v, sizeof(brokers[index_courant].topic_prefix) - 1);
            }
        }
    }

    fclose(fichier);
    return 0;
}

static void initialiser_mqtt(MqttBroker brokers[], int nb_brokers)
{
    mosquitto_lib_init();

    for (int i = 0; i < nb_brokers; ++i) {
        if (!brokers[i].actif || strlen(brokers[i].host) == 0) continue;

        char client_id[64];
        snprintf(client_id, sizeof(client_id), "fanuc_monitor_%d_%ld", i + 1, (long)time(NULL));

        brokers[i].mosq = mosquitto_new(client_id, true, NULL);
        if (!brokers[i].mosq) {
            fprintf(stderr, "Erreur création client MQTT broker %d\n", i + 1);
            continue;
        }

        if (strlen(brokers[i].username) > 0) {
            mosquitto_username_pw_set(brokers[i].mosq, brokers[i].username,
                                     strlen(brokers[i].password) > 0 ? brokers[i].password : NULL);
        }

        int rc = mosquitto_connect(brokers[i].mosq, brokers[i].host, brokers[i].port, 60);
        if (rc == MOSQ_ERR_SUCCESS) {
            brokers[i].connecte = 1;
            mosquitto_loop_start(brokers[i].mosq);
            printf("MQTT %d connecte a %s:%d\n", i + 1, brokers[i].host, brokers[i].port);
        } else {
            fprintf(stderr, "Echec connexion MQTT %d (%s:%d): %s\n",
                    i + 1, brokers[i].host, brokers[i].port, mosquitto_strerror(rc));
        }
    }
}

static void fermer_mqtt(MqttBroker brokers[], int nb_brokers)
{
    for (int i = 0; i < nb_brokers; ++i) {
        if (brokers[i].mosq) {
            if (brokers[i].connecte) {
                mosquitto_loop_stop(brokers[i].mosq, true);
                mosquitto_disconnect(brokers[i].mosq);
            }
            mosquitto_destroy(brokers[i].mosq);
            brokers[i].mosq = NULL;
        }
    }
    mosquitto_lib_cleanup();
}

static const char *libelle_etat(short v)
{
    switch (v) {
        case 0: return "RESET";
        case 1: return "STOP";
        case 2: return "HOLD";
        case 3: return "START";
        case 4: return "MSTR";
        default: return "UNKNOWN";
    }
}

static const char *libelle_alarme(short v)
{
    switch (v) {
        case 0: return "NONE";
        case 1: return "CNC";
        case 8: return "PMC";
        default: return "YES";
    }
}

static const char *libelle_mode(short v)
{
    switch (v) {
        case 0:  return "MDI";
        case 1:  return "MEM";
        case 3:  return "EDIT";
        case 4:  return "HANDLE";
        case 5:  return "JOG";
        case 8:  return "INC";
        case 9:  return "REF";
        case 10: return "REMOTE";
        default: return "UNKNOWN";
    }
}

static const char *libelle_urgence(short v)
{
    switch (v) {
        case 0: return "CLEAR";
        case 1: return "EMERGENCY";
        case 2: return "RESET";
        default: return "UNKNOWN";
    }
}

/* Publie une valeur avec retain = true */
static void publier_valeur(MqttBroker brokers[], int nb_brokers,
                           const char *base_topic, const char *subtopic, const char *valeur)
{
    char full_topic[256];
    for (int i = 0; i < nb_brokers; ++i) {
        if (brokers[i].actif && brokers[i].connecte && brokers[i].mosq) {
            snprintf(full_topic, sizeof(full_topic), "%s/%s/%s", brokers[i].topic_prefix, base_topic, subtopic);
            mosquitto_publish(brokers[i].mosq, NULL, full_topic, (int)strlen(valeur), valeur, 0, true);
        }
    }
}

/* Lecture du compteur de pièces (paramètre Fanuc #6711) */
static long lire_compteur_pieces(unsigned short handle)
{
    IODBPSD param;
    short ret = cnc_rdparam(handle, 6711, 0, sizeof(param), &param);
    if (ret == EW_OK) {
        return param.u.ldata;
    }
    return -1;
}

/* Lecture du temps de cycle usiné (paramètre Fanuc #6758 en ms ou #6757) */
static long lire_temps_cycle_ms(unsigned short handle)
{
    IODBPSD param;
    /* #6758: Temps de cycle précédent / actuel en ms */
    short ret = cnc_rdparam(handle, 6758, 0, sizeof(param), &param);
    if (ret == EW_OK) {
        return param.u.ldata;
    }
    /* Fallback sur #6757 si non disponible */
    ret = cnc_rdparam(handle, 6757, 0, sizeof(param), &param);
    if (ret == EW_OK) {
        return param.u.ldata;
    }
    return -1;
}

/* Publication arborescente (sans topic "status", et "ip" positionné en dernier) */
static void publier_arborescence_mqtt(MqttBroker brokers[], int nb_brokers,
                                      const Machine *m, const ResultatMachine *res)
{
    char machine_topic[32];
    snprintf(machine_topic, sizeof(machine_topic), "CNC_%02d", m->numero);

    char buffer[64];

    // 1. Online
    if (!res->lu) {
        publier_valeur(brokers, nb_brokers, machine_topic, "online", "false");
        // IP toujours en dernier
        publier_valeur(brokers, nb_brokers, machine_topic, "ip", m->host);
        return;
    }

    publier_valeur(brokers, nb_brokers, machine_topic, "online", "true");

    // 2. Run / Cycle status
    snprintf(buffer, sizeof(buffer), "%d", res->status.run);
    publier_valeur(brokers, nb_brokers, machine_topic, "run", buffer);
    publier_valeur(brokers, nb_brokers, machine_topic, "run_label", libelle_etat(res->status.run));

    // 3. Alarme
    snprintf(buffer, sizeof(buffer), "%d", res->status.alarm);
    publier_valeur(brokers, nb_brokers, machine_topic, "alarm", buffer);
    publier_valeur(brokers, nb_brokers, machine_topic, "alarm_label", libelle_alarme(res->status.alarm));

    // 4. Mode de fonctionnement
    snprintf(buffer, sizeof(buffer), "%d", res->status.aut);
    publier_valeur(brokers, nb_brokers, machine_topic, "aut", buffer);
    publier_valeur(brokers, nb_brokers, machine_topic, "aut_label", libelle_mode(res->status.aut));

    // 5. Édition programme
    snprintf(buffer, sizeof(buffer), "%d", res->status.edit);
    publier_valeur(brokers, nb_brokers, machine_topic, "edit", buffer);

    // 6. Fonction auxiliaire active (M/S/T/B)
    snprintf(buffer, sizeof(buffer), "%d", res->status.mstb);
    publier_valeur(brokers, nb_brokers, machine_topic, "mstb", buffer);

    // 7. Urgence
    snprintf(buffer, sizeof(buffer), "%d", res->status.emergency);
    publier_valeur(brokers, nb_brokers, machine_topic, "emergency", buffer);
    publier_valeur(brokers, nb_brokers, machine_topic, "emergency_label", libelle_urgence(res->status.emergency));

    // 8. Compteur de pièces
    if (res->part_count >= 0) {
        snprintf(buffer, sizeof(buffer), "%ld", res->part_count);
        publier_valeur(brokers, nb_brokers, machine_topic, "part_count", buffer);
    }

    // 9. Temps de cycle
    if (res->cycle_time_ms >= 0) {
        long total_sec = res->cycle_time_ms / 1000;
        long minutes = total_sec / 60;
        long secondes = total_sec % 60;

        // Temps brut en secondes
        snprintf(buffer, sizeof(buffer), "%ld", total_sec);
        publier_valeur(brokers, nb_brokers, machine_topic, "cycle_time_sec", buffer);

        // Temps formaté (MM:SS)
        snprintf(buffer, sizeof(buffer), "%02ld:%02ld", minutes, secondes);
        publier_valeur(brokers, nb_brokers, machine_topic, "cycle_time_formatted", buffer);
    }

    // 10. IP (publié en dernier)
    publier_valeur(brokers, nb_brokers, machine_topic, "ip", m->host);
}

int main(void)
{
    Machine machines[MAX_MACHINES] = {0};
    ResultatMachine resultats[MAX_MACHINES] = {0};
    MqttBroker brokers[MAX_MQTT_BROKERS];

    signal(SIGINT, gerer_signal_arret);
#ifndef _WIN32
    signal(SIGTERM, gerer_signal_arret);
#endif

    int nombre_machines = charger_configuration_cnc(machines, MAX_MACHINES);
    if (nombre_machines < 0) {
        return EXIT_FAILURE;
    }

    charger_configuration_mqtt(brokers, MAX_MQTT_BROKERS);
    initialiser_mqtt(brokers, MAX_MQTT_BROKERS);

#ifndef _WIN32
    if (cnc_startupprocess(0, "focas.log") != EW_OK) {
        fprintf(stderr, "Impossible d'initialiser focas.log\n");
    }
#endif

    for (int i = 0; i < nombre_machines; ++i) {
        printf("Connexion à CNC_%02d (%s:%d)...\n",
               machines[i].numero, machines[i].host, MACHINE_PORT);
        fflush(stdout);

        int code = cnc_allclibhndl3(machines[i].host, MACHINE_PORT, 10, &machines[i].handle);
        if (code != EW_OK) {
            fprintf(stderr, "Échec de connexion à CNC_%02d (code %d)\n",
                    machines[i].numero, code);
            continue;
        }
        machines[i].handle_alloue = 1;
    }

    while (continuer) {
        memset(resultats, 0, sizeof(resultats));

        for (int i = 0; i < nombre_machines && continuer; ++i) {
            if (!machines[i].handle_alloue) {
                int code = cnc_allclibhndl3(machines[i].host, MACHINE_PORT, 10, &machines[i].handle);
                if (code == EW_OK) {
                    machines[i].handle_alloue = 1;
                }
            }

            if (machines[i].handle_alloue) {
                resultats[i].status = (ODBST){0};
                int code = cnc_statinfo(machines[i].handle, &resultats[i].status);
                if (code == EW_OK) {
                    resultats[i].lu = 1;

                    // Lecture du compteur de pièces et du temps de cycle
                    resultats[i].part_count = lire_compteur_pieces(machines[i].handle);
                    resultats[i].cycle_time_ms = lire_temps_cycle_ms(machines[i].handle);
                } else {
                    fprintf(stderr, "Échec de lecture CNC_%02d (code %d)\n",
                            machines[i].numero, code);
                    cnc_freelibhndl(machines[i].handle);
                    machines[i].handle_alloue = 0;
                }
            }

            // Publication MQTT arborescente
            publier_arborescence_mqtt(brokers, MAX_MQTT_BROKERS, &machines[i], &resultats[i]);
        }

        if (continuer) {
            attendre_60_secondes();
        }
    }

    for (int i = 0; i < nombre_machines; ++i) {
        if (machines[i].handle_alloue) {
            cnc_freelibhndl(machines[i].handle);
        }
    }

#ifndef _WIN32
    cnc_exitprocess();
#endif

    fermer_mqtt(brokers, MAX_MQTT_BROKERS);

    return EXIT_SUCCESS;
}