#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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
#define MAX_MQTT_BROKERS 3
#define CONFIG_CNC_FILE "/config/liste-cnc.conf"
#define CONFIG_MQTT_FILE "/config/mqtt.conf"
#define MAX_LINE_LENGTH 256
#define MAX_HOST_LENGTH 64
#define MAX_STR_LENGTH 128

static volatile sig_atomic_t continuer = 1;

typedef struct {
    int actif;
    char host[MAX_HOST_LENGTH];
    int port;
    char username[MAX_STR_LENGTH];
    char password[MAX_STR_LENGTH];
    char topic_prefix[MAX_STR_LENGTH];
    struct mosquitto *mosq;
    int connecte;
} MqttBroker;

typedef struct {
    int numero;
    char host[MAX_HOST_LENGTH];
    unsigned short handle;
    int handle_alloue;
} Machine;

typedef struct {
    int lu;
    ODBST status;
    int32_t part_count;
    int32_t cycle_time_sec;
    char cycle_time_formatted[16];
} ResultatMachine;

static void log_horodatage(void)
{
    time_t maintenant = time(NULL);
    struct tm *instant = localtime(&maintenant);
    if (instant != NULL) {
        printf("[%04d-%02d-%02d %02d:%02d:%02d] ",
               instant->tm_year + 1900, instant->tm_mon + 1, instant->tm_mday,
               instant->tm_hour, instant->tm_min, instant->tm_sec);
    } else {
        printf("[----/--/-- --:--:--] ");
    }
}

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

/* Lecture robuste compatible 32 bits et 64 bits d'un paramètre 32-bit Fanuc */
static int32_t lire_param_int32(unsigned short handle, short num_param)
{
    /*
     * En protocole FOCAS pour un paramètre de type "long" (4 octets) :
     * Header = datano (2 octets) + type (2 octets) = 4 octets
     * Data   = valeur (4 octets)
     * Longueur transmise au contrôleur = 4 + 4 = 8 octets
     */
    uint8_t buffer[64];
    memset(buffer, 0, sizeof(buffer));

    short length = 4 + 4;
    short ret = cnc_rdparam(handle, num_param, 0, length, (IODBPSD *)buffer);

    if (ret == EW_OK) {
        /*
         * La valeur utile est située après les 4 octets d'en-tête (datano et type).
         * On copie exactement 4 octets vers un int32_t pour éviter tout écrasement 64 bits.
         */
        int32_t val = 0;
        memcpy(&val, buffer + 4, sizeof(int32_t));
        return val;
    }

    return -1;
}

/* Compteur de pièces (paramètre #6711, repli sur #6712 si négatif) */
static int32_t lire_compteur_pieces(unsigned short handle)
{
    int32_t pieces = lire_param_int32(handle, 6711);
    if (pieces < 0) {
        pieces = lire_param_int32(handle, 6712);
    }
    return pieces >= 0 ? pieces : 0;
}

/* Temps de cycle en secondes (paramètre #6758 donne des millisecondes) */
static int32_t lire_temps_cycle_sec(unsigned short handle)
{
    int32_t ms = lire_param_int32(handle, 6758);
    if (ms <= 0) {
        ms = lire_param_int32(handle, 6757);
    }
    if (ms > 0) {
        return ms / 1000;
    }
    return 0;
}

static void formater_temps_cycle(int32_t total_sec, char *dest, size_t taille)
{
    if (total_sec <= 0) {
        snprintf(dest, taille, "00:00");
        return;
    }
    int32_t heures = total_sec / 3600;
    int32_t minutes = (total_sec % 3600) / 60;
    int32_t secondes = total_sec % 60;

    if (heures > 0) {
        snprintf(dest, taille, "%02d:%02d:%02d", heures, minutes, secondes);
    } else {
        snprintf(dest, taille, "%02d:%02d", minutes, secondes);
    }
}

/* Libellés conformes */
static const char *libelle_run(short v)
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

static const char *libelle_alarm(short v)
{
    switch (v) {
        case 0: return "NONE";
        case 1: return "CNC";
        case 8: return "PMC";
        default: return "ALARM";
    }
}

static const char *libelle_aut(short v)
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

static const char *libelle_emergency(short v)
{
    switch (v) {
        case 0: return "CLEAR";
        case 1: return "EMERGENCY";
        case 2: return "RESET";
        default: return "UNKNOWN";
    }
}

/* Lecture configuration CNC */
static int charger_configuration_cnc(Machine machines[], int capacite)
{
    FILE *fichier = fopen(CONFIG_CNC_FILE, "r");
    if (fichier == NULL) {
        log_horodatage();
        fprintf(stderr, "Erreur : Impossible d'ouvrir %s : %s\n",
                CONFIG_CNC_FILE, strerror(errno));
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

        int numero = 0;
        int position = 0;
        if (sscanf(p, " CNC_%d : %n", &numero, &position) != 1 ||
            position == 0 || numero < 1 || numero > 999) {
            continue;
        }

        p += position;
        while (isspace((unsigned char)*p)) ++p;

        char host[MAX_HOST_LENGTH];
        size_t longueur = 0;

        if (*p == '"') {
            ++p;
            char *fin = strchr(p, '"');
            if (fin == NULL) continue;
            longueur = (size_t)(fin - p);
            if (longueur >= sizeof(host)) continue;
            memcpy(host, p, longueur);
            host[longueur] = '\0';
        } else {
            char *fin = p;
            while (*fin != '\0' && !isspace((unsigned char)*fin) && *fin != '#') fin++;
            longueur = (size_t)(fin - p);
            if (longueur >= sizeof(host)) continue;
            memcpy(host, p, longueur);
            host[longueur] = '\0';
        }

        if (nombre >= capacite) break;

        machines[nombre].numero = numero;
        strncpy(machines[nombre].host, host, sizeof(machines[nombre].host) - 1);
        machines[nombre].handle = 0;
        machines[nombre].handle_alloue = 0;
        ++nombre;
    }

    fclose(fichier);
    return nombre;
}

/* Lecture configuration MQTT */
static int charger_configuration_mqtt(MqttBroker brokers[], int capacite)
{
    FILE *fichier = fopen(CONFIG_MQTT_FILE, "r");
    if (fichier == NULL) {
        log_horodatage();
        fprintf(stderr, "Erreur : Impossible d'ouvrir %s : %s\n",
                CONFIG_MQTT_FILE, strerror(errno));
        return -1;
    }

    char ligne[MAX_LINE_LENGTH];
    int index_courant = -1;
    int nb_charges = 0;

    for (int i = 0; i < capacite; ++i) {
        brokers[i].actif = 0;
        brokers[i].port = 1883;
        brokers[i].connecte = 0;
        brokers[i].mosq = NULL;
        brokers[i].username[0] = '\0';
        brokers[i].password[0] = '\0';
        strncpy(brokers[i].topic_prefix, "juradec/cnc", sizeof(brokers[i].topic_prefix) - 1);
    }

    while (fgets(ligne, sizeof(ligne), fichier) != NULL) {
        char *p = ligne;
        while (isspace((unsigned char)*p)) ++p;
        if (*p == '\0' || *p == '#' || *p == '\n') continue;

        if (*p == '[') {
            int num_broker = 0;
            if (sscanf(p, "[MQTT_%d]", &num_broker) == 1 && num_broker >= 1 && num_broker <= capacite) {
                index_courant = num_broker - 1;
                brokers[index_courant].actif = 1;
                nb_charges++;
            } else {
                index_courant = -1;
            }
            continue;
        }

        if (index_courant >= 0 && index_courant < capacite) {
            char cle[64], valeur[128];
            if (sscanf(p, "%63[^= \t] = %127[^\r\n]", cle, valeur) == 2 ||
                sscanf(p, "%63[^= \t]=%127[^\r\n]", cle, valeur) == 2) {
                // Trim valeur
                char *v = valeur;
                while (isspace((unsigned char)*v)) ++v;
                char *fin = v + strlen(v) - 1;
                while (fin > v && isspace((unsigned char)*fin)) {
                    *fin = '\0';
                    --fin;
                }

                if (strcmp(cle, "host") == 0) {
                    strncpy(brokers[index_courant].host, v, sizeof(brokers[index_courant].host) - 1);
                } else if (strcmp(cle, "port") == 0) {
                    brokers[index_courant].port = atoi(v);
                } else if (strcmp(cle, "username") == 0) {
                    strncpy(brokers[index_courant].username, v, sizeof(brokers[index_courant].username) - 1);
                } else if (strcmp(cle, "password") == 0) {
                    strncpy(brokers[index_courant].password, v, sizeof(brokers[index_courant].password) - 1);
                } else if (strcmp(cle, "topic_prefix") == 0) {
                    strncpy(brokers[index_courant].topic_prefix, v, sizeof(brokers[index_courant].topic_prefix) - 1);
                }
            }
        }
    }

    fclose(fichier);
    return nb_charges;
}

static void initialiser_mqtt(MqttBroker brokers[], int nb_brokers)
{
    mosquitto_lib_init();

    for (int i = 0; i < nb_brokers; ++i) {
        if (!brokers[i].actif || brokers[i].host[0] == '\0') continue;

        char client_id[64];
        snprintf(client_id, sizeof(client_id), "fanuc_monitor_%d_%ld", i + 1, (long)time(NULL));

        brokers[i].mosq = mosquitto_new(client_id, true, NULL);
        if (!brokers[i].mosq) {
            log_horodatage();
            fprintf(stderr, "MQTT_%d : Échec création instance mosquitto\n", i + 1);
            continue;
        }

        if (brokers[i].username[0] != '\0') {
            mosquitto_username_pw_set(brokers[i].mosq, brokers[i].username,
                                      brokers[i].password[0] != '\0' ? brokers[i].password : NULL);
        }

        int code = mosquitto_connect(brokers[i].mosq, brokers[i].host, brokers[i].port, 60);
        if (code == MOSQ_ERR_SUCCESS) {
            brokers[i].connecte = 1;
            mosquitto_loop_start(brokers[i].mosq);
            log_horodatage();
            printf("MQTT_%d : Connecté au broker %s:%d\n", i + 1, brokers[i].host, brokers[i].port);
        } else {
            brokers[i].connecte = 0;
            log_horodatage();
            fprintf(stderr, "MQTT_%d : Connexion impossible à %s:%d (code %d)\n",
                    i + 1, brokers[i].host, brokers[i].port, code);
        }
    }
}

static void verifier_reconnexion_mqtt(MqttBroker brokers[], int nb_brokers)
{
    for (int i = 0; i < nb_brokers; ++i) {
        if (!brokers[i].actif || brokers[i].host[0] == '\0') continue;

        if (!brokers[i].connecte) {
            int code = mosquitto_reconnect(brokers[i].mosq);
            if (code == MOSQ_ERR_SUCCESS) {
                brokers[i].connecte = 1;
                mosquitto_loop_start(brokers[i].mosq);
                log_horodatage();
                printf("MQTT_%d : Reconnecté avec succès à %s:%d\n", i + 1, brokers[i].host, brokers[i].port);
            }
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
            brokers[i].connecte = 0;
            log_horodatage();
            printf("MQTT_%d : Session déconnectée et libérée\n", i + 1);
        }
    }
    mosquitto_lib_cleanup();
}

static void publier_feuille(MqttBroker *b, const char *machine_id, const char *feuille, const char *valeur)
{
    if (!b->connecte || !b->mosq) return;

    char topic[256];
    snprintf(topic, sizeof(topic), "%s/%s/%s", b->topic_prefix, machine_id, feuille);
    mosquitto_publish(b->mosq, NULL, topic, (int)strlen(valeur), valeur, 0, true);
}

/* Publication arborescente avec ip en dernière position */
static void publier_arborescence_mqtt(MqttBroker brokers[], int nb_brokers,
                                      const Machine *mach, const ResultatMachine *res)
{
    char machine_id[32];
    snprintf(machine_id, sizeof(machine_id), "CNC_%02d", mach->numero);
    char tampon[64];

    for (int b = 0; b < nb_brokers; ++b) {
        if (!brokers[b].connecte) continue;

        if (!res->lu) {
            publier_feuille(&brokers[b], machine_id, "online", "false");
            publier_feuille(&brokers[b], machine_id, "ip", mach->host);
            continue;
        }

        publier_feuille(&brokers[b], machine_id, "online", "true");

        snprintf(tampon, sizeof(tampon), "%d", (int)res->status.run);
        publier_feuille(&brokers[b], machine_id, "run", tampon);
        publier_feuille(&brokers[b], machine_id, "run_label", libelle_run(res->status.run));

        snprintf(tampon, sizeof(tampon), "%d", (int)res->status.alarm);
        publier_feuille(&brokers[b], machine_id, "alarm", tampon);
        publier_feuille(&brokers[b], machine_id, "alarm_label", libelle_alarm(res->status.alarm));

        snprintf(tampon, sizeof(tampon), "%d", (int)res->status.aut);
        publier_feuille(&brokers[b], machine_id, "aut", tampon);
        publier_feuille(&brokers[b], machine_id, "aut_label", libelle_aut(res->status.aut));

        snprintf(tampon, sizeof(tampon), "%d", (int)res->status.edit);
        publier_feuille(&brokers[b], machine_id, "edit", tampon);

        snprintf(tampon, sizeof(tampon), "%d", (int)res->status.mstb);
        publier_feuille(&brokers[b], machine_id, "mstb", tampon);

        snprintf(tampon, sizeof(tampon), "%d", (int)res->status.emergency);
        publier_feuille(&brokers[b], machine_id, "emergency", tampon);
        publier_feuille(&brokers[b], machine_id, "emergency_label", libelle_emergency(res->status.emergency));

        snprintf(tampon, sizeof(tampon), "%d", (int)res->part_count);
        publier_feuille(&brokers[b], machine_id, "part_count", tampon);

        snprintf(tampon, sizeof(tampon), "%d", (int)res->cycle_time_sec);
        publier_feuille(&brokers[b], machine_id, "cycle_time_sec", tampon);

        publier_feuille(&brokers[b], machine_id, "cycle_time_formatted", res->cycle_time_formatted);

        /* IP toujours en dernier */
        publier_feuille(&brokers[b], machine_id, "ip", mach->host);
    }
}

int main(void)
{
    Machine machines[MAX_MACHINES] = {0};
    MqttBroker brokers[MAX_MQTT_BROKERS] = {0};

    signal(SIGINT, gerer_signal_arret);
#ifndef _WIN32
    signal(SIGTERM, gerer_signal_arret);
#endif

    int nombre_machines = charger_configuration_cnc(machines, MAX_MACHINES);
    if (nombre_machines <= 0) {
        log_horodatage();
        fprintf(stderr, "Aucune CNC trouvée dans la configuration.\n");
        return EXIT_FAILURE;
    }

    charger_configuration_mqtt(brokers, MAX_MQTT_BROKERS);
    initialiser_mqtt(brokers, MAX_MQTT_BROKERS);

#ifndef _WIN32
    if (cnc_startupprocess(0, "focas.log") != EW_OK) {
        log_horodatage();
        fprintf(stderr, "FOCAS : Impossible d'initialiser le processus (focas.log)\n");
        return EXIT_FAILURE;
    }
#endif

    /* Connexions initiales FOCAS */
    for (int i = 0; i < nombre_machines; ++i) {
        int code = cnc_allclibhndl3(machines[i].host, MACHINE_PORT, 10, &machines[i].handle);
        if (code == EW_OK) {
            machines[i].handle_alloue = 1;
            log_horodatage();
            printf("FOCAS : Connecté à CNC_%02d (%s:%d)\n",
                   machines[i].numero, machines[i].host, MACHINE_PORT);
        } else {
            machines[i].handle_alloue = 0;
            log_horodatage();
            fprintf(stderr, "FOCAS : Échec initial connexion à CNC_%02d (%s:%d) (code %d)\n",
                    machines[i].numero, machines[i].host, MACHINE_PORT, code);
        }
    }

    ResultatMachine resultats[MAX_MACHINES];

    /* Boucle principale */
    while (continuer) {
        verifier_reconnexion_mqtt(brokers, MAX_MQTT_BROKERS);
        memset(resultats, 0, sizeof(resultats));

        for (int i = 0; i < nombre_machines && continuer; ++i) {
            if (!machines[i].handle_alloue) {
                int code = cnc_allclibhndl3(machines[i].host, MACHINE_PORT, 5, &machines[i].handle);
                if (code == EW_OK) {
                    machines[i].handle_alloue = 1;
                    log_horodatage();
                    printf("FOCAS : Reconnecté à CNC_%02d (%s:%d)\n",
                           machines[i].numero, machines[i].host, MACHINE_PORT);
                }
            }

            if (machines[i].handle_alloue) {
                resultats[i].status = (ODBST){0};
                int code = cnc_statinfo(machines[i].handle, &resultats[i].status);
                if (code == EW_OK) {
                    resultats[i].lu = 1;
                    resultats[i].part_count = lire_compteur_pieces(machines[i].handle);
                    resultats[i].cycle_time_sec = lire_temps_cycle_sec(machines[i].handle);
                    formater_temps_cycle(resultats[i].cycle_time_sec,
                                         resultats[i].cycle_time_formatted,
                                         sizeof(resultats[i].cycle_time_formatted));
                } else {
                    log_horodatage();
                    fprintf(stderr, "FOCAS : Déconnexion ou erreur de lecture CNC_%02d (%s) (code %d)\n",
                            machines[i].numero, machines[i].host, code);
                    cnc_freelibhndl(machines[i].handle);
                    machines[i].handle_alloue = 0;
                }
            }

            publier_arborescence_mqtt(brokers, MAX_MQTT_BROKERS, &machines[i], &resultats[i]);
        }

        fflush(stdout);
        fflush(stderr);

        if (continuer) {
            attendre_60_secondes();
        }
    }

    /* Nettoyage final */
    for (int i = 0; i < nombre_machines; ++i) {
        if (machines[i].handle_alloue) {
            cnc_freelibhndl(machines[i].handle);
            machines[i].handle_alloue = 0;
            log_horodatage();
            printf("FOCAS : Déconnecté proprement de CNC_%02d\n", machines[i].numero);
        }
    }

#ifndef _WIN32
    cnc_exitprocess();
#endif

    fermer_mqtt(brokers, MAX_MQTT_BROKERS);
    return EXIT_SUCCESS;
}