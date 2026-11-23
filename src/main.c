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

/* Supprime les espaces en début et fin de chaîne */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

/* Lecture du fichier /config/mqtt.conf */
static int charger_configuration_mqtt(MqttBroker brokers[], int max_brokers)
{
    FILE *fichier = fopen(CONFIG_MQTT_FILE, "r");
    if (fichier == NULL) {
        fprintf(stderr, "Impossible d'ouvrir %s : %s\n",
                CONFIG_MQTT_FILE, strerror(errno));
        return -1;
    }

    char ligne[MAX_LINE_LENGTH];
    int broker_idx = -1;

    for (int i = 0; i < max_brokers; ++i) {
        memset(&brokers[i], 0, sizeof(MqttBroker));
        brokers[i].port = 1883;
        strcpy(brokers[i].topic_prefix, "cnc");
    }

    while (fgets(ligne, sizeof(ligne), fichier) != NULL) {
        char *p = trim(ligne);

        if (*p == '\0' || *p == '#' || *p == ';') {
            continue;
        }

        if (p[0] == '[' && p[strlen(p) - 1] == ']') {
            p[strlen(p) - 1] = '\0';
            p++;
            int num = 0;
            if (sscanf(p, "MQTT_%d", &num) == 1 && num >= 1 && num <= max_brokers) {
                broker_idx = num - 1;
                brokers[broker_idx].actif = 1;
            } else {
                broker_idx = -1;
            }
            continue;
        }

        if (broker_idx < 0 || broker_idx >= max_brokers) {
            continue;
        }

        char *separateur = strchr(p, '=');
        if (!separateur) continue;

        *separateur = '\0';
        char *cle = trim(p);
        char *val = trim(separateur + 1);

        if (strcmp(cle, "host") == 0) {
            strncpy(brokers[broker_idx].host, val, sizeof(brokers[broker_idx].host) - 1);
        } else if (strcmp(cle, "port") == 0) {
            brokers[broker_idx].port = atoi(val);
        } else if (strcmp(cle, "username") == 0) {
            strncpy(brokers[broker_idx].username, val, sizeof(brokers[broker_idx].username) - 1);
        } else if (strcmp(cle, "password") == 0) {
            strncpy(brokers[broker_idx].password, val, sizeof(brokers[broker_idx].password) - 1);
        } else if (strcmp(cle, "topic_prefix") == 0) {
            strncpy(brokers[broker_idx].topic_prefix, val, sizeof(brokers[broker_idx].topic_prefix) - 1);
        }
    }

    fclose(fichier);

    int count = 0;
    for (int i = 0; i < max_brokers; ++i) {
        if (brokers[i].actif && strlen(brokers[i].host) > 0) {
            count++;
        } else {
            brokers[i].actif = 0;
        }
    }

    return count;
}

/* Initialise les connexions aux brokers MQTT configurés */
static void init_mqtt_brokers(MqttBroker brokers[], int max_brokers)
{
    mosquitto_lib_init();

    for (int i = 0; i < max_brokers; ++i) {
        if (!brokers[i].actif) continue;

        char client_id[32];
        snprintf(client_id, sizeof(client_id), "fanuc_agent_%d_%d", i + 1, (int)time(NULL));

        brokers[i].mosq = mosquitto_new(client_id, true, NULL);
        if (!brokers[i].mosq) {
            fprintf(stderr, "[MQTT_%d] Erreur d'allocation Mosquitto\n", i + 1);
            continue;
        }

        if (strlen(brokers[i].username) > 0) {
            mosquitto_username_pw_set(brokers[i].mosq,
                                      brokers[i].username,
                                      strlen(brokers[i].password) > 0 ? brokers[i].password : NULL);
        }

        int res = mosquitto_connect(brokers[i].mosq, brokers[i].host, brokers[i].port, 60);
        if (res == MOSQ_ERR_SUCCESS) {
            printf("[MQTT_%d] Connecté à %s:%d\n", i + 1, brokers[i].host, brokers[i].port);
            mosquitto_loop_start(brokers[i].mosq);
            brokers[i].connecte = 1;
        } else {
            fprintf(stderr, "[MQTT_%d] Échec connexion à %s:%d (%s)\n",
                    i + 1, brokers[i].host, brokers[i].port, mosquitto_strerror(res));
        }
    }
}

/* Déconnexion propre des brokers */
static void fermer_mqtt_brokers(MqttBroker brokers[], int max_brokers)
{
    for (int i = 0; i < max_brokers; ++i) {
        if (brokers[i].mosq) {
            mosquitto_loop_stop(brokers[i].mosq, true);
            mosquitto_disconnect(brokers[i].mosq);
            mosquitto_destroy(brokers[i].mosq);
            brokers[i].mosq = NULL;
            brokers[i].connecte = 0;
        }
    }
    mosquitto_lib_cleanup();
}

/* Lit liste-cnc.conf */
static int charger_configuration(Machine machines[], int capacite)
{
    FILE *fichier = fopen(CONFIG_CNC_FILE, "r");
    if (fichier == NULL) {
        fprintf(stderr, "Impossible d’ouvrir %s : %s\n", CONFIG_CNC_FILE, strerror(errno));
        return -1;
    }

    char ligne[MAX_LINE_LENGTH];
    int nombre = 0;
    int numero_ligne = 0;

    while (fgets(ligne, sizeof(ligne), fichier) != NULL) {
        ++numero_ligne;
        char *p = ligne;
        while (isspace((unsigned char)*p)) ++p;

        if (*p == '\0' || *p == '#' || *p == '\n') continue;

        int numero;
        int position = 0;
        if (sscanf(p, " CNC_%d : %n", &numero, &position) != 1 || position == 0 || numero < 1 || numero > 999) {
            fprintf(stderr, "Format invalide dans %s, ligne %d\n", CONFIG_CNC_FILE, numero_ligne);
            fclose(fichier);
            return -1;
        }

        p += position;
        while (isspace((unsigned char)*p)) ++p;

        char host[MAX_HOST_LENGTH];
        size_t longueur = 0;

        if (*p == '"') {
            ++p;
            char *fin = strchr(p, '"');
            if (fin == NULL) {
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
            while (*fin != '\0' && *fin != '\n' && *fin != '\r' && *fin != '#' && !isspace((unsigned char)*fin)) {
                ++fin;
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
            fprintf(stderr, "Trop de machines : max %d\n", capacite);
            fclose(fichier);
            return -1;
        }

        machines[nombre].numero = numero;
        strcpy(machines[nombre].host, host);
        ++nombre;
    }

    fclose(fichier);
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

static const char *libelle_urgence_court(short v)
{
    switch (v) {
        case 0: return "Non";
        case 1: return "Oui";
        case 2: return "Reinit";
        default: return "Inconnu";
    }
}

/* Publie l'état d'une machine vers tous les brokers configurés */
static void publier_mqtt(MqttBroker brokers[], int max_brokers,
                         const Machine *machine, const ResultatMachine *res)
{
    char topic[256];
    char payload[512];
    time_t now = time(NULL);

    if (res->lu) {
        snprintf(payload, sizeof(payload),
                 "{"
                 "\"machine\":\"CNC_%02d\","
                 "\"ip\":\"%s\","
                 "\"online\":true,"
                 "\"run\":%d,"
                 "\"run_label\":\"%s\","
                 "\"alarm\":%d,"
                 "\"alarm_label\":\"%s\","
                 "\"aut_mode\":%d,"
                 "\"aut_label\":\"%s\","
                 "\"edit\":%d,"
                 "\"mstb\":%d,"
                 "\"emergency\":%d,"
                 "\"emergency_label\":\"%s\","
                 "\"timestamp\":%ld"
                 "}",
                 machine->numero,
                 machine->host,
                 (int)res->status.run, libelle_etat(res->status.run),
                 (int)res->status.alarm, libelle_alarme_court(res->status.alarm),
                 (int)res->status.aut, libelle_mode_court(res->status.aut),
                 (int)res->status.edit,
                 (int)res->status.mstb,
                 (int)res->status.emergency, libelle_urgence_court(res->status.emergency),
                 (long)now);
    } else {
        snprintf(payload, sizeof(payload),
                 "{"
                 "\"machine\":\"CNC_%02d\","
                 "\"ip\":\"%s\","
                 "\"online\":false,"
                 "\"timestamp\":%ld"
                 "}",
                 machine->numero,
                 machine->host,
                 (long)now);
    }

    for (int i = 0; i < max_brokers; ++i) {
        if (!brokers[i].actif || !brokers[i].mosq || !brokers[i].connecte) {
            continue;
        }

        snprintf(topic, sizeof(topic), "%s/CNC_%02d/status",
                 brokers[i].topic_prefix, machine->numero);

        int code = mosquitto_publish(brokers[i].mosq, NULL, topic,
                                     (int)strlen(payload), payload,
                                     0, false);
        if (code != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "[MQTT_%d] Erreur d'envoi CNC_%02d : %s\n",
                    i + 1, machine->numero, mosquitto_strerror(code));
        }
    }
}

int main(void)
{
    Machine machines[MAX_MACHINES] = {0};
    ResultatMachine resultats[MAX_MACHINES] = {0};
    MqttBroker brokers[MAX_MQTT_BROKERS];
    int ret = EXIT_SUCCESS;

    signal(SIGINT, gerer_signal_arret);
#ifndef _WIN32
    signal(SIGTERM, gerer_signal_arret);
#endif

    int nombre_machines = charger_configuration(machines, MAX_MACHINES);
    if (nombre_machines < 0) {
        return EXIT_FAILURE;
    }

    int nombre_brokers = charger_configuration_mqtt(brokers, MAX_MQTT_BROKERS);
    printf("%d machine(s) et %d broker(s) MQTT charges.\n", nombre_machines, nombre_brokers);

    init_mqtt_brokers(brokers, MAX_MQTT_BROKERS);

#ifndef _WIN32
    if (cnc_startupprocess(0, "focas.log") != EW_OK) {
        fprintf(stderr, "Impossible de creer focas.log\n");
        fermer_mqtt_brokers(brokers, MAX_MQTT_BROKERS);
        return EXIT_FAILURE;
    }
#endif

    for (int i = 0; i < nombre_machines; ++i) {
        printf("Connexion a CNC_%02d (%s:%d)...\n", machines[i].numero, machines[i].host, MACHINE_PORT);
        fflush(stdout);

        int code = cnc_allclibhndl3(machines[i].host, MACHINE_PORT, 10, &machines[i].handle);
        if (code != EW_OK) {
            fprintf(stderr, "Echec de connexion a CNC_%02d (code %d)\n", machines[i].numero, code);
            continue;
        }
        machines[i].handle_alloue = 1;
    }

    while (continuer) {
        memset(resultats, 0, sizeof(resultats));

        for (int i = 0; i < nombre_machines && continuer; ++i) {
            if (!machines[i].handle_alloue) {
                publier_mqtt(brokers, MAX_MQTT_BROKERS, &machines[i], &resultats[i]);
                continue;
            }

            resultats[i].status = (ODBST){0};
            int code = cnc_statinfo(machines[i].handle, &resultats[i].status);
            if (code == EW_OK) {
                resultats[i].lu = 1;
            } else {
                fprintf(stderr, "Echec de lecture CNC_%02d (code %d)\n", machines[i].numero, code);
            }

            publier_mqtt(brokers, MAX_MQTT_BROKERS, &machines[i], &resultats[i]);
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

    fermer_mqtt_brokers(brokers, MAX_MQTT_BROKERS);

    return ret;
}