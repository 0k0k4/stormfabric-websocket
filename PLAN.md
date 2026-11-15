# Plan — mod `websocket` (ws:// et wss:// pour les scripts Stormworks)

Mod natif StormFabric qui donne aux scripts Lua un client WebSocket complet vers
n'importe quelle URL `ws://` ou `wss://`. Suit `AGENTS.md` (§1 règles, §7 addon,
§8 microcontrôleur, §9 storage, §10 build). Développé dans le dossier `testmod/` (ignoré
par git) du dépôt StormFabric, puis déplacé dans ce dépôt.

---

## 1. Contraintes qui dictent l'architecture

| Contrainte (AGENTS.md / SDK) | Conséquence |
|---|---|
| Pas de `luaState` gardé, pas de référence Lua stockable (pas de registry) | **API par polling** : impossible de garder une fonction Lua de rappel. Le script appelle `websocket.poll()` dans `onTick`. |
| Les callbacks Lua tournent sur le thread de simulation / VM, ne doivent pas bloquer | `connect`/`send` ne font qu'**enfiler** ; tout le réseau vit dans des threads de travail. |
| Chaînes Lua ≤ `SW_LUA_MAX_STRING_BYTES` (1 MiB) | Messages entrants plafonnés à 1 MiB (réassemblage des fragments, au-delà : fermeture 1009). |
| Pas d'adresse du jeu | Le mod n'utilise **que** `host->api` + l'API Windows. Aucun hook. |
| `registerAddonLibrary` / `extras` = build 1.15.23 seulement ; `registerLibrary` = les deux builds | Bibliothèque addon si `SW_CAP_LUA_ADDON`, bibliothèque microcontrôleur toujours (si `extras->pushString` est accepté dans une VM micro — **à vérifier phase 4**). |
| Le mod peut être déchargé à chaud | `SwModUnload` ferme tous les handles et **joint** tous les threads avant de rendre la main. |

## 2. Choix technique : WinHTTP WebSocket

`winhttp.dll` (système, Windows 8+) implémente nativement le protocole :
`WinHttpOpen` → `WinHttpConnect` → requête GET avec
`WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET` → `WinHttpWebSocketCompleteUpgrade`
→ `WinHttpWebSocketSend/Receive/Close`.

- **wss** : TLS via Schannel (certificats du magasin Windows), rien à embarquer.
- Ping/pong, masquage, en-têtes `Sec-WebSocket-*` gérés par WinHTTP.
- Proxy système : `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`.
- Keepalive : `WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL`.
- Aucune dépendance externe → compatible `/MT`, un seul DLL à livrer (lier `winhttp.lib`).

Alternative écartée : implémentation maison (Winsock + Schannel + framing RFC 6455) —
beaucoup plus de code et de risques TLS pour aucun gain ici.

## 3. API Lua exposée (table `websocket`)

Identique côté addon (`script.lua`) et microcontrôleur, sauf mention.

| Fonction | Retour | Rôle |
|---|---|---|
| `websocket.connect(url [, options])` | `id` ou `nil, err` | Ouvre une connexion asynchrone. `options` = chaîne `"k=v;k=v"` (pas de table côté micro) : `protocol=`, `header=Nom: valeur` (répétable), `insecure=1` (certificat non vérifié, dev), `timeout=` (s). |
| `websocket.send(id, data [, binary])` | `bool` | Enfile un message texte (défaut) ou binaire. Refusé si pas `open` ou file pleine. |
| `websocket.poll([id])` | `id, type, data, extra` ou `nil` | Défile **un** événement (global ou d'une connexion) : `"open"`, `"message"` (`extra` = `binary` bool), `"close"` (`data` = raison, `extra` = code), `"error"` (`data` = message). |
| `websocket.receive(id)` | `data, binary` ou `nil` | Raccourci : prochain message seulement. |
| `websocket.state(id)` | `"connecting"\|"open"\|"closing"\|"closed"` | État courant. |
| `websocket.close(id [, code, reason])` | `bool` | Fermeture propre (défaut 1000). |
| `websocket.pending(id)` | `n` | Événements en attente. |
| `websocket.version()` | `string` | Version du mod (sert de test de présence). |

Usage type (addon) :

```lua
local sock
function onCreate()
    if websocket then sock = websocket.connect("wss://echo.websocket.org") end
end
function onTick()
    if not websocket then return end
    while true do
        local id, kind, data, extra = websocket.poll()
        if not id then break end
        if kind == "open" then websocket.send(id, "hello")
        elseif kind == "message" then server.announce("ws", data)
        elseif kind == "close" or kind == "error" then server.announce("ws", kind .. " " .. tostring(data)) end
    end
end
```

## 4. Architecture interne

```text
stormfabric-websocket/
├── PLAN.md
├── mod.json                 id "websocket", main "websocket.dll"
├── src/
│   ├── ws_client.h/.cpp     cœur WinHTTP, indépendant du SDK (testable hors jeu)
│   ├── ws_registry.h/.cpp   table id -> connexion, limites, nettoyage
│   └── ws_mod.cpp           SwModInit/Unload, bindings Lua (addon + micro)
├── build.bat                cl /MT ... winhttp.lib
├── tests/
│   ├── ws_selftest.cpp      exe console qui pilote ws_client (sans le jeu)
│   └── echo_server.py       serveur ws local + wss auto-signé (paquet `websockets`)
└── addon_test/script.lua    addon de test en jeu (+ playlist.xml minimal)
```

**Connexion** (`WsConnection`) :
- `std::thread` réception : handshake, puis boucle `WinHttpWebSocketReceive`,
  réassemblage des fragments, push dans la file entrante (mutex).
- `std::thread` émission : attend une `condition_variable`, envoie la file sortante.
  WinHTTP autorise un send et un receive simultanés sur le même handle.
- États atomiques ; dernier message d'erreur (`GetLastError` → texte).
- `close()` : `WinHttpWebSocketClose`, puis `WinHttpCloseHandle` débloque le receive.

**Registre** :
- `id` = entier croissant (jamais réutilisé), jamais un pointeur.
- Limites (configurables) : 32 connexions, 256 événements en file par connexion
  (au-delà : drop + événement `"error" "queue overflow"`), 1 MiB par message, 4 MiB
  en attente d'envoi par connexion.
- **Ramasse-miettes** : une connexion jamais pollée depuis N s (défaut 30) est fermée
  — couvre les VM détruites (fin de monde, véhicule supprimé, `?reload_scripts`)
  puisque le mod ne peut pas identifier une VM.
- Fin de monde : si `SW_CAP_GAME_EVENTS`, abonnement à `onDestroy` → tout fermer.
  Le nettoyage par inactivité reste le filet de sécurité sur l'autre build.

**Configuration** (`host->api->storage`, clé `websocket.cfg`) : limites ci-dessus,
`allow_hosts` (défaut `*` = toute URL, comme demandé ; liste optionnelle pour
restreindre), `log_connections=1`. Chaque `connect` est écrit dans
`StormworksModLoader.log` (hôte + port, jamais les en-têtes).

## 5. Phases

### Phase 0 — Préparation ✅
- `testmod/` ajouté au `.gitignore`, dossier `testmod/websocket/` créé.

### Phase 1 — Cœur WinHTTP hors jeu ✅ (2026-11-15)
Fait : `src/ws_client.{h,cpp}`, `tests/ws_selftest.cpp`, `tests/echo_server.py`
(stdlib + `cryptography`, lancer **sans** `-I`), `build.bat` → `build/ws_selftest.exe`.
Résultat : 16/16 tests (15 locaux + `--online` sur `wss://echo.websocket.org`), répétés
4 fois, 0 thread restant. Constats : fermer un handle WinHTTP depuis un autre thread
annule bien un appel bloquant (connexion vers 10.255.255.1 annulée < 1,2 s) ; l'écho
continue au-delà du `timeout` de handshake (timeout de réception désactivé à l'upgrade).
Comportements fixés : `send` est refusé tant que l'état n'est pas `open` ; un `Close()`
pendant le handshake produit un `close` avec le code demandé, sans `error` ; tout échec
réseau produit `error` puis `close` 1006.

1. `ws_client.cpp` : parsing d'URL (`WinHttpCrackUrl`, refuser tout schéma autre que
   ws/wss), handshake, threads rx/tx, fermeture, erreurs.
2. `ws_selftest.exe` + `echo_server.py` : écho texte/binaire, message 1 MiB fragmenté,
   fermeture côté serveur (code + raison), serveur absent, DNS invalide, wss
   auto-signé refusé puis accepté avec `insecure=1`, `wss://echo.websocket.org`.
3. Critère : tous les cas passent, aucun thread restant après destruction (compteur).

### Phase 2 — Registre et limites ✅ (2026-11-15)
Fait : `src/ws_registry.{h,cpp}` — ids croissants, limites, `allow_hosts`, ramasse-miettes
par inactivité, `CloseOwner`/`CloseAll` qui joignent les threads hors verrou, config
`key=value` (`websocket.cfg`, stockée par le loader dans `mods/websocket/config/websocket.cfg.bin`).
Ajout : **isolation par VM** sans garder de `luaState` — au premier appel d'une VM, le mod
pose un jeton numérique `websocket.__owner` dans la table `websocket` de cette VM ;
chaque connexion appartient à un jeton, `poll()` global ne voit que les siennes. Si la
table globale a été retirée, repli sur le propriétaire partagé 0.
Fin de monde : compteur `SW_EVENT_ADDON_LUA_CREATED/DESTROYED` → `CloseAll` quand il
retombe à 0 (en plus du ramasse-miettes).

### Phase 3 — Mod addon (build 1.15.23) ✅ validé en jeu (2026-11-15, v0.2.2)
Vérifié dans le jeu : `ws://127.0.0.1:8765` (open, écho `bonjour`, fermeture 1000) et
`wss://echo.websocket.org` (open, message d'accueil du serveur, écho, `TEST OK`,
fermeture 1000). Événements reçus par `poll()` depuis `onTick`, connexions lancées
depuis `onCustomCommand` : l'isolation par VM fonctionne dans le vrai jeu.
Non vérifié en jeu : fin de monde (CloseAll), `?reload_scripts`, plusieurs addons.
Fait : `src/ws_mod.cpp`, `mod.json` (v0.2.0, désactivé par défaut), `build.bat` →
`build/websocket.dll`, `tests/mod_test.cpp` (faux hôte : mini pile Lua avec globales par
VM, faux runtime/storage, vérifie qu'aucun appel ne laisse de valeur sur la pile) :
7/7 ; `ws_selftest` toujours 15/15. `addon_test/script.lua` (commandes `?ws`) et
`install.bat` (copie dans `H:\Jeux\StormworksV1.15.23\Stormworks\mods\websocket`,
ajoute `websocket=1`).
1er test en jeu (2026-11-15) : `connect` OK mais jamais `open` — le jeton de VM était lu
avec `argNumber(L, -1)`, or le loader refuse les index négatifs dans `argNumber/argBool/
argString` (renvoie la valeur par défaut) → nouveau propriétaire à chaque appel. Corrigé
(index positif), faux hôte aligné sur le loader, test de régression ajouté ; ids et codes
renvoyés en entiers Lua. Une connexion fermée est libérée quand `poll` rend son `close`
(sinon par le ramasse-miettes). Reste : le test en jeu ci-dessous.

Plan d'origine :
- `ws_mod.cpp` selon le gabarit §7 : vérif `sdkVersion`, `abiVersion`,
  `structSize` de `SwLuaApi.registerAddonLibrary` **et** `extras->pushString`,
  `SW_CAP_LUA_ADDON`. `checkStack` avant chaque push, retour = nombre de résultats.
- Lecture des chaînes via `extras->copyString` (préserve binaire/NUL) avec buffer de
  1 MiB alloué, pas `argString`.
- `SwModUnload` → `closeAll()`.
- Test en jeu : `addon_test/script.lua` ; vérifier le log, l'écho, la fermeture du monde
  (connexions fermées), le rechargement des scripts.

### Phase 4 — Bibliothèque microcontrôleur ✅ validé en jeu (2026-11-15, v0.3.0)
Vérifié dans le jeu : `mc_test.lua` sur un écran affiche `open`, `tx 7 rx 7`, `mc 7`
(envoi toutes les 2 s, écho reçu par `poll(id)` dans la VM du micro).
Incident pendant le test, sans lien avec le mod : gel au spawn du véhicule causé par une
sélection QuickEdit dans la console du loader (écriture console bloquée sous le verrou
du log) — corrigé dans le loader.
Fait : même table `websocket` via `registerLibrary` (les deux builds) ; bibliothèque
addon seulement si `SW_CAP_LUA_ADDON`. Le mod démarre si au moins une des deux est
enregistrée. `extras->getGlobal` devient optionnel (sans lui : propriétaire partagé 0).
Config `microcontrollers=1` (0 = addons seulement, à cause des véhicules du Workshop).
Côté loader, `HasLuaContext` (src/native_lua.cpp) accepte toute VM de `g_liveLuaVms`
pendant un callback de bibliothèque : `extras` et le jeton `__owner` devraient marcher en
micro (à confirmer en jeu). `mod_test` 8/8 (test micro : écho, isolation entre micros et
addon). Script de test : `addon_test/mc_test.lua` (écran + sorties 1).

Plan d'origine :
- Même table via `registerLibrary` (§8). Vérifier en jeu que `extras->pushString`/
  `copyString` fonctionnent dans une VM micro ; sinon exposer en micro une API
  numérique réduite et le documenter.
- Vérifier le nettoyage quand le véhicule est supprimé (ramasse-miettes).

### Phase 5 — Durcissement et livraison — fait hors jeu ✅, 3 vérifications en jeu ⏳
Fait (2026-11-15) : relecture avec la checklist `AGENTS.md` §12 (pas d'adresse du jeu,
`structSize`/capacités vérifiés, aucun `luaState` gardé, `checkStack` avant chaque push,
pas de fichier du jeu, `mod.json` valide) ; corrigé un déséquilibre de pile dans
`connect` si le message d'erreur ne pouvait pas être poussé. `/W4 /WX` sans
avertissement. Déchargement avec trafic couvert par `mod_test` (cycles init/unload,
11 connexions ouvertes ou en cours, 0 thread restant). `README.md` du mod (API,
options, config, sécurité, dépannage). Tests : `mod_test` 8/8, `ws_selftest` 17/17.
À vérifier en jeu : (1) fermeture du monde avec une connexion ouverte → journal
`last addon VM destroyed: closing connections` ; (2) `?reload_scripts` avec une connexion
ouverte → `released idle connection` ~30 s après. (3) Déchargement depuis le menu MODS :
impossible en pleine partie (menu principal seulement, où aucune connexion n'existe) ;
couvert hors jeu par `mod_test`.

Plan d'origine :
- Revue de la checklist AGENTS.md §12, `/W4` sans avertissement.
- Déchargement à chaud depuis le menu MODS avec connexions ouvertes (pas de crash,
  threads joints).
- `README.md` du mod (API, options, limites, sécurité) et `install.bat` local.

## 6. Risques / points ouverts

- **`error 8` (mémoire insuffisante) à `WinHttpSendRequest` dans le jeu** — constaté le
  2026-11-15 vers `wss://echo.websocket.org`, hors jeu tout passe. Écarté :
  - le proxy `iphlpapi.dll` de StormFabric (selftest 16/16 avec ce DLL à côté) ;
  - l'émulateur Steam : `steamclient64.ccl.dll` pose via Detours des hooks sur `connect`,
    `WSAConnect`, `sendto` (+ `WinHttpConnect` seulement si winhttp est déjà chargé à son
    attache), sauf si `disable_lan_only.txt` existe (créé dans `steam_settings`). WinHTTP
    passe par `ConnectEx`, non intercepté ; l'erreur a persisté avec le fichier.
  - le proxy WinHTTP (v0.2.1 : option `proxy=auto|direct|system`, échec identique en
    direct ; l'option est conservée) ; Windhawk (présent aussi hors jeu, où tout marche).
  **Cause (v0.2.2)** : le DLL du mod s'appelait `websocket.dll`, comme
  `System32\websocket.dll` que WinHTTP charge en différé pour le protocole WebSocket.
  Windows lie une demande par nom de base au module déjà chargé → WinHTTP recevait le
  mod, sans `WebSocketBeginClientHandshake` → erreur 8. Trouvé en lisant la mémoire du jeu
  (seul le DLL du mod chargé sous ce nom) et reproduit hors jeu (préchargement d'un
  `websocket.dll` quelconque → même erreur). Corrigé : DLL renommé `sf_websocket.dll`, et
  `SwModInit` refuse de démarrer si le nom du DLL existe dans System32.

- **Sécurité** : n'importe quel addon (workshop compris) pourra ouvrir des connexions
  sortantes et exfiltrer des données. Mitigation : log systématique, `allow_hosts`
  configurable, mod `enabled_by_default: false`.
- **Multijoueur** : les addons tournent sur l'hôte → seules les connexions de l'hôte.
  Les microcontrôleurs tournent chez chaque client : comportement à observer.
- **VM micro et `extras`** : non garanti par la doc actuelle (phase 4).
- **`SwModUnload` et le verrou du loader** : confirmer que joindre des threads y est
  sûr (la doc dit que les callbacks sont déjà révoqués) ; sinon fermer les handles et
  détacher avec un compteur de références sur le DLL.
- **Pas de push d'événements** vers Lua : la latence minimale est d'un tick (~16 ms).
