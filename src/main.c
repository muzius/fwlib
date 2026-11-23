#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <limits.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "fwlib32.h"
#include "MQTTClient.h"

#define MACHINE_PORT 8193
#define MAX_MACHINES 100
#define CONFIG_FILE "/config/liste-cnc.conf"
#define MQTT_CONFIG_FILE "/config/mqtt.conf"
#define MAX_LINE_LENGTH 256
#define MAX_HOST_LENGTH 64
#define MAX_MQTT_HOST_LENGTH 128
#define MAX_USERNAME_LENGTH 128
#define MAX_PASSWORD_LENGTH 128
#define MAX_TOPIC_LENGTH 192
#define MAX_BROKERS 3
#define MQTT_CLIENT_ID "fanuc-cnc-monitor"

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
    char host[MAX_MQTT_HOST_LENGTH];
    int port;
    char username[MAX_USERNAME_LENGTH];
    char password[MAX_PASSWORD_LENGTH];
    char topic_prefix[MAX_TOPIC_LENGTH];
    int username_present;
    int password_present;
    int actif;
    MQTTClient client;
} Broker;

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

static char *rogner(char *texte)
{
    while (isspace((unsigned char)*texte)) {
        ++texte;
    }

    char *fin = texte + strlen(texte);
    while (fin > texte && isspace((unsigned char)fin[-1])) {
        --fin;
    }
    *fin = '\0';

    return texte;
}

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

        char *p = rogner(ligne);
        if (*p == '\0' || *p == '#') {
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

        p = rogner(p + position);

        char *fin = p;
        if (*p == '"') {
            ++p;
            fin = strchr(p, '"');
            if (fin == NULL) {
                fprintf(stderr, "Adresse sans guillemet fermant, ligne %d\n",
                        numero_ligne);
                fclose(fichier);
                return -1;
            }
        } else {
            fin = p;
            while (*fin != '\0' && *fin != '#' &&
                   !isspace((unsigned char)*fin)) {
                ++fin;
            }
        }

        size_t longueur = (size_t)(fin - p);
        if (longueur == 0 || longueur >= MAX_HOST_LENGTH) {
            fprintf(stderr, "Adresse invalide, ligne %d\n", numero_ligne);
            fclose(fichier);
            return -1;
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
        memcpy(machines[nombre].host, p, longueur);
        machines[nombre].host[longueur] = '\0';
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

static int copier_valeur(char *destination, size_t taille, const char *valeur)
{
    size_t longueur = strlen(valeur);
    if (longueur == 0 || longueur >= taille) {
        return -1;
    }

    memcpy(destination, valeur, longueur + 1);
    return 0;
}

/* Lit les blocs [MQTT_1] à [MQTT_3] du fichier mqtt.conf. */
static int charger_brokers(Broker brokers[], int capacite)
{
    FILE *fichier = fopen(MQTT_CONFIG_FILE, "r");
    if (fichier == NULL) {
        fprintf(stderr, "Impossible d’ouvrir %s : %s\n",
                MQTT_CONFIG_FILE, strerror(errno));
        return -1;
    }

    char ligne[MAX_LINE_LENGTH];
    int nombre = 0;
    int numero_ligne = 0;
    Broker *courant = NULL;

    while (fgets(ligne, sizeof(ligne), fichier) != NULL) {
        ++numero_ligne;
        char *p = rogner(ligne);

        if (*p == '\0' || *p == '#') {
            continue;
        }

        if (*p == '[') {
            char nom[32];
            int numero;
            char supplement;

            if (sscanf(p, "[MQTT_%d]%c", &numero, &supplement) != 1 ||
                numero < 1 || numero > MAX_BROKERS) {
                fprintf(stderr, "Section MQTT invalide, ligne %d\n",
                        numero_ligne);
                fclose(fichier);
                return -1;
            }

            if (nombre >= capacite) {
                fprintf(stderr, "Trop de brokers MQTT (maximum %d)\n",
                        capacite);
                fclose(fichier);
                return -1;
            }

            (void)nom;
            courant = &brokers[nombre++];
            memset(courant, 0, sizeof(*courant));
            courant->port = 1883;
            continue;
        }

        if (courant == NULL) {
            fprintf(stderr, "Paramètre hors section MQTT, ligne %d\n",
                    numero_ligne);
            fclose(fichier);
            return -1;
        }

        char *egal = strchr(p, '=');
        if (egal == NULL) {
            fprintf(stderr, "Paramètre MQTT invalide, ligne %d\n",
                    numero_ligne);
            fclose(fichier);
            return -1;
        }

        *egal = '\0';
        char *cle = rogner(p);
        char *valeur = rogner(egal + 1);

        if (strcmp(cle, "host") == 0) {
            if (copier_valeur(courant->host, sizeof(courant->host), valeur) != 0)
                goto valeur_invalide;
        } else if (strcmp(cle, "port") == 0) {
            char *fin;
            errno = 0;
            long port = strtol(valeur, &fin, 10);
            if (errno != 0 || *valeur == '\0' || *fin != '\0' ||
                port < 1 || port > 65535)
                goto valeur_invalide;
            courant->port = (int)port;
        } else if (strcmp(cle, "username") == 0) {
            if (copier_valeur(courant->username,
                              sizeof(courant->username), valeur) != 0)
                goto valeur_invalide;
            courant->username_present = 1;
        } else if (strcmp(cle, "password") == 0) {
            if (copier_valeur(courant->password,
                              sizeof(courant->password), valeur) != 0)
                goto valeur_invalide;
            courant->password_present = 1;
        } else if (strcmp(cle, "topic_prefix") == 0) {
            if (copier_valeur(courant->topic_prefix,
                              sizeof(courant->topic_prefix), valeur) != 0)
                goto valeur_invalide;
        } else {
            fprintf(stderr, "Clé MQTT inconnue '%s', ligne %d\n",
                    cle, numero_ligne);
            fclose(fichier);
            return -1;
        }
        continue;

valeur_invalide:
        fprintf(stderr, "Valeur MQTT invalide à la ligne %d\n", numero_ligne);
        fclose(fichier);
        return -1;
    }

    if (ferror(fichier)) {
        fprintf(stderr, "Erreur de lecture de %s\n", MQTT_CONFIG_FILE);
        fclose(fichier);
        return -1;
    }
    fclose(fichier);

    if (nombre == 0) {
        fprintf(stderr, "Aucun broker MQTT configuré dans %s\n",
                MQTT_CONFIG_FILE);
        return -1;
    }

    for (int i = 0; i < nombre; ++i) {
        if (brokers[i].host[0] == '\0' ||
            brokers[i].topic_prefix[0] == '\0' ||
            brokers[i].username_present != brokers[i].password_present) {
            fprintf(stderr,
                    "Configuration incomplète du broker MQTT_%d\n", i + 1);
            return -1;
        }
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
        case 0: return "MDI";
        case 1: return "Mem";
        case 3: return "Edit";
        case 4: return "Handle";
        case 5: return "JOG";
        case 8: return "INC";
        case 9: return "REF";
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
    puts("| Machine | État     | Alarme  | Mode     | Edit  | M/S/T/B | Urgence  | IP             | Heure    |");

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
                   machines[i].numero, "Erreur", "-", "-", "-", "-", "-",
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
               machines[i].host, heure);
    }

    puts("----------------------------------------------------------------------------------------------------");
}

static void publier_broker(Broker *broker,
                           const Machine machines[],
                           const ResultatMachine resultats[],
                           int nombre_machines)
{
    if (!broker->actif) {
        return;
    }

    for (int i = 0; i < nombre_machines; ++i) {
        char topic[256];
        char payload[768];

        int n = snprintf(topic, sizeof(topic), "%s/CNC_%02d/status",
                         broker->topic_prefix, machines[i].numero);
        if (n < 0 || (size_t)n >= sizeof(topic)) {
            fprintf(stderr, "Topic MQTT trop long pour CNC_%02d\n",
                    machines[i].numero);
            continue;
        }

        if (resultats[i].lu) {
            const ODBST *s = &resultats[i].status;
            n = snprintf(payload, sizeof(payload),
                "{\"machine\":\"CNC_%02d\",\"ip\":\"%s\","
                "\"connected\":true,\"run\":%d,\"run_text\":\"%s\","
                "\"alarm\":%d,\"alarm_text\":\"%s\","
                "\"mode\":%d,\"mode_text\":\"%s\","
                "\"edit\":%d,\"mstb\":%d,\"emergency\":%d,"
                "\"emergency_text\":\"%s\",\"timestamp\":%lld}",
                machines[i].numero, machines[i].host,
                (int)s->run, libelle_etat(s->run),
                (int)s->alarm, libelle_alarme_court(s->alarm),
                (int)s->aut, libelle_mode_court(s->aut),
                (int)s->edit, (int)s->mstb,
                (int)s->emergency, libelle_urgence_court(s->emergency),
                (long long)time(NULL));
        } else {
            n = snprintf(payload, sizeof(payload),
                "{\"machine\":\"CNC_%02d\",\"ip\":\"%s\","
                "\"connected\":false,\"timestamp\":%lld}",
                machines[i].numero, machines[i].host,
                (long long)time(NULL));
        }

        if (n < 0 || (size_t)n >= sizeof(payload)) {
            fprintf(stderr, "Payload MQTT trop long pour CNC_%02d\n",
                    machines[i].numero);
            continue;
        }

        MQTTClient_message message = MQTTClient_message_initializer;
        message.payload = payload;
        message.payloadlen = (int)strlen(payload);
        message.qos = 0;
        message.retained = 1;

        MQTTClient_deliveryToken token;
        int code = MQTTClient_publishMessage(broker->client, topic,
                                             &message, &token);
        if (code != MQTTCLIENT_SUCCESS) {
            fprintf(stderr, "Échec publication MQTT vers %s, code %d\n",
                    topic, code);
        }
    }

    MQTTClient_yield();
}

int main(void)
{
    Machine machines[MAX_MACHINES] = {0};
    ResultatMachine resultats[MAX_MACHINES] = {0};
    Broker brokers[MAX_BROKERS] = {0};
    int nombre_brokers = 0;
    int ret = EXIT_SUCCESS;

    signal(SIGINT, gerer_signal_arret);
#ifndef _WIN32
    signal(SIGTERM, gerer_signal_arret);
#endif

    int nombre_machines = charger_configuration(machines, MAX_MACHINES);
    if (nombre_machines < 0) {
        return EXIT_FAILURE;
    }

    nombre_brokers = charger_brokers(brokers, MAX_BROKERS);
    if (nombre_brokers < 0) {
        return EXIT_FAILURE;
    }

    for (int i = 0; i < nombre_brokers; ++i) {
        char uri[192];
        snprintf(uri, sizeof(uri), "tcp://%s:%d",
                 brokers[i].host, brokers[i].port);

        MQTTClient_create(&brokers[i].client, uri, MQTT_CLIENT_ID,
                          MQTTCLIENT_PERSISTENCE_NONE, NULL);

        MQTTClient_connectOptions options = MQTTClient_connectOptions_initializer;
        options.keepAliveInterval = 20;
        options.cleansession = 1;
        if (brokers[i].username_present) {
            options.username = brokers[i].username;
            options.password = brokers[i].password;
        }

        int code = MQTTClient_connect(brokers[i].client, &options);
        if (code == MQTTCLIENT_SUCCESS) {
            brokers[i].actif = 1;
            printf("Connecté au broker MQTT %s\n", uri);
        } else {
            fprintf(stderr, "Connexion MQTT échouée vers %s (code %d)\n",
                    uri, code);
            ret = EXIT_FAILURE;
        }
    }

#ifndef _WIN32
    if (cnc_startupprocess(0, "focas.log") != EW_OK) {
        fprintf(stderr, "Impossible de créer focas.log\n");
        ret = EXIT_FAILURE;
        goto nettoyage_mqtt;
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

            int code = cnc_statinfo(machines[i].handle,
                                    &resultats[i].status);
            if (code == EW_OK) {
                resultats[i].lu = 1;
            } else {
                fprintf(stderr, "Échec de lecture de CNC_%02d (code %d)\n",
                        machines[i].numero, code);
            }
        }

        afficher_tableau(machines, resultats, nombre_machines);

        for (int i = 0; i < nombre_brokers; ++i) {
            publier_broker(&brokers[i], machines, resultats, nombre_machines);
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
                fprintf(stderr, "Échec de libération du handle CNC_%02d (code %d)\n",
                        machines[i].numero, code);
                ret = EXIT_FAILURE;
            }
        }
    }

#ifndef _WIN32
    cnc_exitprocess();
#endif

nettoyage_mqtt:
    for (int i = 0; i < nombre_brokers; ++i) {
        if (brokers[i].client) {
            if (brokers[i].actif) {
                MQTTClient_disconnect(brokers[i].client, 1000);
            }
            MQTTClient_destroy(&brokers[i].client);
        }
    }

    return ret;
}