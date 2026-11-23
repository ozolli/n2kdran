# Brief pour Claude : n2kdran web (+ n2k-mux en option) sur un HALPI2

Ce document est destiné à l'assistant (Claude Code) qui aidera le propriétaire d'un
HALPI2 (Hat Labs, Raspberry Pi CM5, interface NMEA 2000 intégrée en `can0`) à
installer ces outils. Ils ont été écrits pour un autre bateau (catamaran CM50,
PC de bord Ubuntu « o3nav ») : rien n'a encore été essayé sur un HALPI, sur HaLOS
ni sur ce bus. Vérifier chaque point sur place avant d'agir, et demander au
propriétaire avant toute action irréversible ou touchant le bus.

## Ce que fait chaque outil

- **n2kdran web** (ce dossier) : répétiteur d'instruments dans un navigateur
  (rose, laylines, cibles de polaire, AIS). `relay.py` (Python 3, bibliothèque
  standard seule) sert la page et lui transmet le flux N2K par WebSocket ; le
  cœur de calcul est en WebAssembly (`n2k.js`, `n2k.wasm`, déjà compilés,
  indépendants du processeur). Mode d'emploi : `LISEZMOI.txt` ; manuel complet :
  `n2kdran-manuel.pdf` (chapitre 9 pour la version web).
- **n2k-mux** (https://github.com/ozolli/n2k-mux, Apache 2.0) : multiplexeur
  NMEA 2000 en C. Il choisit la meilleure source par donnée (arbitrage) et
  redistribue en N2K (vcan0, YDRAW TCP 2700) et en 0183 (TCP 10110). Le dépôt a
  son propre `CLAUDE.md` et un README en français ; le § 2.6 traite du HALPI2.

## Étape 1 — n2kdran web seul, en lecture seule (recommandé d'abord)

Aucun risque pour le bus : la source `can:can0` ouvre un socket CAN brut en
lecture, n'émet jamais, et cohabite avec Signal K sur la même interface.

1. Vérifier : `python3 --version` (≥ 3.7), `ip -details link show can0`
   (UP, bitrate 250000 ; sur HaLOS l'interface est préconfigurée), et
   `candump -n 20 can0` si can-utils est installé.
2. Ports libres : `ss -ltn`. La page écoute sur **8090** par défaut ; 3000 est
   souvent pris par Signal K, 9090 par Cockpit (HaLOS). Changer avec `--http`.
3. Essai : `python3 relay.py --n2k can:can0`, puis http://<halpi>:8090/.
   Indicateur « N2K » vert = données reçues ; double clic = toutes les données.
4. Service utilisateur : section 2 de `LISEZMOI.txt` (`DIR=` = ce dossier).
5. Faire charger la **polaire du bateau** (réglages, fichier qtVlm .pol/.csv) :
   celle du CM50 est installée par défaut.

Limite connue de ce mode : sans n2k-mux, le cœur de n2kdran choisit lui-même
une source par donnée (préférée + bascule si elle se tait), sans arbitrage fin.

## Étape 2 (facultative) — n2k-mux

Lire d'abord `CLAUDE.md` et `README.fr.md` du dépôt n2k-mux. Points propres à ce
bateau, à régler AVANT d'activer le service `n2k-mux-can` :

1. **n2k-mux émet sur le bus.** Le lanceur `n2k-mux-can-run` passe
   `--tx-can $CANIF --src-addr $SRC_ADDR` : toutes les 30 s, des ISO Request
   (PGN 59904, demandant 60928 et 126996) partent de l'adresse `SRC_ADDR`,
   **0 par défaut, sans revendication d'adresse (address claim)**. Sur un bus
   où un appareil occupe déjà l'adresse 0 (fréquent : traceur, pilote…), c'est
   un conflit d'adresse. Avant tout démarrage : relever les adresses occupées
   (`candump can0`, ou l'onglet Appareils de n2k-mux-web, ou Signal K) et poser
   `SRC_ADDR=<adresse libre>` dans `/etc/default/n2k-mux`. Signal K (canboatjs)
   revendique lui aussi une adresse : ne pas la reprendre. Il n'existe pas
   aujourd'hui d'option pour couper cette émission ; si le propriétaire la
   refuse, il faut modifier le lanceur (retirer `--tx-can`) et en mesurer les
   conséquences (l'arbitrage résout les identités grâce à ces requêtes).
2. **`can0` est déjà géré par HaLOS.** Le service tente
   `ip link set can0 up type can bitrate 250000` (erreurs tolérées) : sans effet
   si l'interface est déjà montée à 250 kbit/s. Vérifier qu'il ne casse pas la
   configuration de HaLOS, et ne pas changer le débit.
3. **Ports** : interface web de n2k-mux sur **8080** (`PORT=` dans
   `/etc/default/n2k-mux`), YDRAW 2700, 0183 10110, n2kd 2597–2602. Vérifier
   qu'ils sont libres. `WEB_AUTH` est obligatoire si l'interface écoute sur le LAN.
4. **Dépendances à compiler sur ARM** (non essayé sur aarch64) : canboat
   (`analyzer`, `n2kd`, `candump2analyzer` ; binaires dans `rel/linux-<arch>/`,
   chemins à reporter dans `/etc/default/n2k-mux`) et kplex (absent des dépôts
   Debian). `make test` dans n2k-mux doit passer avant `make install`.
5. **HaLOS** : système orienté conteneurs (Cockpit). Vérifier que l'installation
   de services système (`/usr/local/bin`, `/etc/systemd/system`) y est possible
   et durable avant de commencer.
6. **Configuration** (`/etc/n2k-mux/n2k-mux.ini`, § 3 du README) : les
   identités d'appareils et les priorités sont celles de CE bus, à construire
   avec l'onglet Appareils de l'interface web. Ne pas reprendre celles de
   l'exemple.

Une fois n2k-mux en marche : relancer le relais de n2kdran web avec
`--n2k 127.0.0.1:2700` (flux arbitré) au lieu de `can:can0`.

## À ne pas faire

- Ne rien émettre sur le bus sans l'accord du propriétaire (étape 2, point 1).
- Ne pas modifier le débit ni la configuration de `can0` gérée par HaLOS.
- Ne pas désactiver ou reconfigurer Signal K sans l'accord du propriétaire.
- Ne pas publier de capture du bus (positions, MMSI de tiers) dans un dépôt public.
