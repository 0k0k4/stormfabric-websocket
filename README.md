# WebSocket — mod StormFabric

Client WebSocket complet (`ws://` et `wss://`, n'importe quelle URL) pour les scripts Lua
de Stormworks : **addons/missions** (`script.lua`) et **microcontrôleurs**. Le réseau est
fait par WinHTTP (Windows 8+), dans des threads à part : un appel Lua ne bloque jamais le jeu.

| | Addons (`script.lua`) | Microcontrôleurs |
|---|---|---|
| Build 1.15.23 | ✅ | ✅ |
| Build 1.15.18 | — | ✅ |

Validé en jeu le 2026-11-15 (v0.3.0) : écho local `ws://`, `wss://echo.websocket.org`,
microcontrôleur avec écran.

Nécessite le mod loader [StormFabric](https://github.com/0k0k4/StormFabric) installé dans
le jeu.

## Installation

1. Fermer Stormworks.
2. `build.bat` (MSVC x64), puis `install.bat <dossier_du_jeu>` : copie `mod.json` et
   `sf_websocket.dll` dans `<jeu>\mods\websocket\` et ajoute `websocket=1` à
   `mods\_modloader.ini`.
3. Lancer par `Launcher.exe`. Le journal `StormworksModLoader.log` doit contenir
   `websocket 0.3.0 ready` et `libraries: addon=yes microcontroller=yes`.

> Le DLL ne doit **jamais** s'appeler `websocket.dll` : c'est le nom d'un DLL de Windows
> que WinHTTP charge pour le protocole WebSocket. Le mod refuse de démarrer si son nom
> existe dans `System32` (voir [`AGENTS.md`](https://github.com/0k0k4/StormFabric/blob/main/AGENTS.md) de
> StormFabric, règle 7).

## API Lua

Toujours tester la table pour que le script marche sans le mod : `if websocket then … end`.

| Fonction | Retour |
|---|---|
| `websocket.connect(url [, options])` | `id`, ou `nil, erreur` |
| `websocket.send(id, data [, binaire])` | `true` / `false` |
| `websocket.poll([id])` | `id, type, data, extra`, ou rien |
| `websocket.receive(id)` | `data, binaire`, ou rien |
| `websocket.state(id)` | `"connecting"`, `"open"`, `"closing"` ou `"closed"` |
| `websocket.close(id [, code [, raison]])` | `true` / `false` |
| `websocket.pending(id)` | nombre d'événements en attente |
| `websocket.version()` | `"0.3.0"` |

**Événements** (`poll`) :

| `type` | `data` | `extra` |
|---|---|---|
| `"open"` | — | — |
| `"message"` | contenu (chaîne d'octets, NUL compris) | `true` si binaire |
| `"error"` | message lisible | code (Win32/WinHTTP ou statut HTTP) |
| `"close"` | raison | code de fermeture (1006 = coupure sans fermeture propre) |

Une erreur est toujours suivie d'un `close`. Une connexion est libérée quand `poll` rend
son `close` ; `state` renvoie alors `"closed"`.

**Options** de `connect`, chaîne `"clé=valeur;clé=valeur"` :

| Clé | Valeur | Effet |
|---|---|---|
| `protocol` | texte | en-tête `Sec-WebSocket-Protocol` |
| `header` | `Nom: valeur` | en-tête HTTP en plus (répétable ; pas de `;` dans la valeur) |
| `insecure` | `0` / `1` | `wss` : accepter un certificat invalide ou auto-signé |
| `timeout` | 1–120 s | délai DNS/connexion/handshake (10 s par défaut) |
| `proxy` | `auto` / `direct` / `system` | `auto` : proxy automatique, puis direct si échec non réseau |

### Exemple — addon

```lua
local sock
function onCreate()
    if websocket then sock = websocket.connect("wss://echo.websocket.org/") end
end

function onTick()
    if not websocket then return end
    while true do
        local id, kind, data, extra = websocket.poll()
        if not id then break end
        if kind == "open" then websocket.send(id, "bonjour")
        elseif kind == "message" then server.announce("ws", data)
        elseif kind == "close" then sock = nil end
    end
end
```

### Exemple — microcontrôleur

Voir `addon_test/mc_test.lua` (état et dernier message sur un écran, envoi toutes les 2 s).

## Isolation et durée de vie

- Chaque VM (un addon, un microcontrôleur) ne voit que **ses** connexions : au premier
  appel, le mod pose un jeton numérique `websocket.__owner` dans la table `websocket` de la
  VM. Ne pas modifier ce champ. Si un script retire la table globale `websocket`, ses
  connexions passent dans un propriétaire partagé.
- Une connexion que personne n'interroge (`poll`, `send`, `state`…) pendant
  `idle_timeout` secondes est fermée : c'est ce qui nettoie après une VM détruite
  (véhicule supprimé, `?reload_scripts`). **Appeler `poll` à chaque tick.**
- À la fermeture du monde (plus aucune VM d'addon), toutes les connexions sont fermées.
- Le déchargement du mod (menu MODS) ferme tout et attend la fin des threads.

## Configuration

Fichier créé au premier lancement : `<jeu>\mods\websocket\config\websocket.cfg.bin`
(texte `clé=valeur`, à modifier jeu fermé ; une valeur invalide fait ignorer tout le
fichier, avec la raison dans le journal).

| Clé | Défaut | Effet |
|---|---|---|
| `allow_hosts` | `*` | hôtes autorisés, séparés par des virgules : `*`, `hote`, `*.domaine` |
| `max_connections` | 32 | connexions simultanées (tous scripts confondus) |
| `idle_timeout` | 30 | secondes sans appel avant fermeture |
| `max_message_bytes` | 1048576 | taille max d'un message reçu (au-delà : fermeture 1009) |
| `max_queued_events` | 256 | événements en attente par connexion (au-delà : messages ignorés + `error`) |
| `max_pending_send_bytes` | 4194304 | octets en attente d'envoi par connexion |
| `log_connections` | 1 | écrire chaque connexion dans le journal |
| `microcontrollers` | 1 | `0` : bibliothèque réservée aux addons |

## Sécurité

N'importe quel script qui reçoit la table peut ouvrir une connexion sortante, y compris
le Lua d'un **véhicule du Workshop**. Le mod est désactivé par défaut (`mod.json`), journalise
chaque connexion (hôte et port seulement, jamais le chemin ni les en-têtes) et permet de
restreindre les hôtes (`allow_hosts`) ou de couper les microcontrôleurs
(`microcontrollers=0`). En multijoueur, les addons tournent chez l'hôte ; les
microcontrôleurs chez chaque joueur qui a le mod.

## Dépannage

| Symptôme | Cause |
|---|---|
| `websocket` est `nil` | mod désactivé, ou script créé avant le chargement du mod (recharger le monde) |
| `error 8 … mémoire insuffisante` | un DLL nommé `websocket.dll` est chargé dans le jeu (ancienne version du mod) |
| `error 12007` / `12029` | nom introuvable / serveur injoignable |
| `error 12175` | certificat refusé (`insecure=1` pour un serveur de test) |
| `error 403` (ou autre statut) | le serveur a refusé l'upgrade WebSocket |
| `host not allowed` | hôte absent de `allow_hosts` |
| connexion fermée seule après ~30 s | le script n'appelle plus `poll` (`idle_timeout`) |

## Développement

- `include/` : en-têtes du SDK StormFabric (licence MIT, `include/LICENSE-StormFabric`).
  Pour compiler avec une autre version : `set SF_SDK=<StormFabric>\include` avant `build.bat`.
- `src/ws_client.*` : client WinHTTP, indépendant du SDK. `src/ws_registry.*` : ids,
  propriétaires, limites, config. `src/ws_mod.cpp` : point d'entrée et API Lua.
- Tests hors jeu (lancer d'abord `python tests/echo_server.py`, sans `-I`) :
  `build\ws_selftest.exe [--online]` (client) et `build\mod_test.exe` (API Lua via un faux
  hôte qui suit les règles du loader).
- Test en jeu : `addon_test/script.lua` (commandes `?ws`) et `addon_test/mc_test.lua`.
