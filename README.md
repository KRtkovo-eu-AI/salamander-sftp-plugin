# SFTP/SCP plugin pro Open Salamander (x64)

Plnohodnotný **SFTP a SCP** klient jako file-system doplněk do [Open Salamander 5.0](https://github.com/OpenSalamander/salamander) (x64). Postavený nad **libssh2 + OpenSSL**, takže umí i moderní kryptografii, kterou původní řešení neuměla.

> Copyright © 2026 Dupl3xx
> Vychází ze SDK demo šablony Open Salamander (SPDX hlavičky ponechány v SDK souborech). Vlastní jádro (`sftpconn.*`, `sftpglue.*`) je autorské.

---

## Co umí

### Protokoly
- **SFTP** (SSH File Transfer Protocol) – výchozí
- **SCP** – výpis přes shell (`ls`/`stat`), přenos přes `libssh2_scp_*`, operace (`mkdir`/`rm`/`mv`/`chmod`) přes shell
- **Nouzové SCP** – když server nemá SFTP subsystém, automatický přechod na SCP

### Kryptografie (díky OpenSSL backendu)
Vše se **sjednává automaticky** podle serveru:
- Výměna klíčů: curve25519-sha256, ECDH (nistp256/384/521), DH group14/16/18
- Typy klíčů: ed25519, ECDSA, RSA (rsa-sha2-256/512)
- Šifry: ChaCha20-Poly1305, AES-GCM, AES-CTR
- Komprese: zlib (volitelně)

### Autentizace
- **Heslo**
- **Privátní klíč** – OpenSSH/PEM i **PuTTY `.ppk`** (RSA + ed25519, v2/v3, vč. zašifrovaných – v2 SHA1/AES, v3 Argon2id; převod přes OpenSSL)
- **Keyboard-interactive** – vč. 2FA (první výzvu vyplní heslem, další se ptá dialogem)

### Bezpečnost
- **Ověření host key** proti `known_hosts` (`%APPDATA%\OpenSalamander-SFTP\known_hosts`)
- Dotaz na důvěru u neznámého serveru (uložit / jen teď / odmítnout), **varování při změně klíče** (MITM)
- Zobrazení otisku SHA256/SHA1, typu klíče a banneru serveru

### Souborové operace
- Procházení (sloupce práva / vlastník / skupina), stahování, nahrávání
- **Progress s rychlostí přenosu**, dotaz na přepsání
- **Obnovení přerušeného přenosu (resume)** – navázání od dané pozice (jen SFTP)
- Mazání, vytvoření adresáře, přejmenování, **změna práv (chmod)**, vlastnosti
- **Úprava souboru na serveru** (F4 – stáhne, otevře editor, po uložení nahraje zpět)
- **Spočítání velikosti** adresáře, příkazová řádka (SSH exec)

### Dialog a správa spojení
- Přihlašovací dialog ve stylu WinSCP (strom kategorií)
- **Uložená spojení**: Nový / Editovat / Odstranit / Přejmenovat / Jako výchozí
- **Oko u hesla** (přepíná hvězdičky ↔ čitelné heslo)
- Pamatuje si protokol, kompresi i SCP fallback per spojení

### Lokalizace
- České i anglické rozhraní; jazykové moduly (`.slg`) pro **všech 11 jazyků** Sally buildu, takže v žádné jazykové verzi Salamandera nevyskočí chyba/výběr jazyka.

---

## Struktura zdrojových kódů (`src/`)

### Vlastní jádro doplňku
| Soubor | Co dělá |
|--------|---------|
| **`sftpconn.h/.cpp`** | **Připojovací vrstva nad libssh2.** Connect (handshake, ověření host key, autentizace heslo/klíč/`.ppk`/KBI), ListDir, Download/Upload (s resume + progress), SCP varianty operací, chmod/stat, host-key verifikace (known_hosts), PuTTY `.ppk` parser + převod na PEM přes OpenSSL. Nezávislé na Salamanderu, testovatelné samostatně. |
| **`sftpglue.h/.cpp`** | **Lepidlo** mezi Salamander FS a `CSftpConnection`. POSIX path helpery (normalizace, join, parent), `SftpEnsureConnected` (registruje callbacky host-key/KBI + cestu known_hosts), vstupní dialog pro KBI, profil spojení `CSftpProfile` + uložená spojení. |

### Integrace do Salamandera (vychází z SDK šablony, výrazně přepsáno)
| Soubor | Co dělá |
|--------|---------|
| `sftp.cpp` | Vstupní bod pluginu, registrace (FS název `dfs`), menu (Upravit soubor F4, Spočítat velikost, Odpojit), načítání/ukládání konfigurace + uložených spojení do registru. |
| `fs1.cpp` | **Přihlašovací dialog** (`ConnectDlgProc`) – strom kategorií, stránky, oko u hesla, správa uložených spojení, vstupní dialog. Definice FS image listu. |
| `fs2.cpp` | **Implementace FS rozhraní** – ChangePath, ListCurrentPath, kopírování z/na FS (s resume a overwrite dialogy), Delete, CreateDir, QuickRename, ChangeAttributes (chmod), ShowProperties, ExecuteCommandLine, ShowSecurityInfo, kontextové menu. |
| `menu.cpp` | Obsluha položek menu pluginu (editace souboru, spočítání velikosti, odpojení). |
| `sftp.h` | Společné deklarace, `CFSData` (práva/vlastník/skupina pro sloupce), konstanty menu. |
| `precomp.h/.cpp` | Předkompilovaná hlavička (vč. `SFTP_QUIET` – potlačí demo dialogy SDK). |
| `dialogs.*`, `archiver.cpp`, `viewer.cpp`, `thumbldr.cpp` | Zbytky SDK šablony – **neaktivní** (plugin je registruje jen jako FS, ne archivér/prohlížeč). |

### Resource a projekt
| Soubor | Co dělá |
|--------|---------|
| `lang/lang.rc`, `lang.rc2`, `lang.rh` | Dialogy (přihlášení, chmod, vstup, progress) a textové řetězce (české). Kompiluje se do `.slg`. |
| `res/fs.ico`, `dir.ico`, `file.ico` | Ikony (FS = převzatá z původního WinSCP doplňku). |
| `versinfo.rh2` | Verze, copyright, popis (version resource). |
| `vcxproj/sftp.vcxproj` | MSVC projekt pluginu (linkuje libssh2 + libcrypto, delay-load, čeština přes `/source-charset:utf-8 /execution-charset:windows-1250`). |
| `vcxproj/lang_sftp.vcxproj` | Projekt jazykového modulu `.slg`. |

---

## Jak to funguje (tok připojení)

1. Uživatel otevře cestu na FS (`dfs:`) → zobrazí se **přihlašovací dialog** (`fs1.cpp`).
2. Po potvrzení `fs2.cpp::ChangePath` zavolá `SftpEnsureConnected` (`sftpglue.cpp`).
3. `CSftpConnection::Connect` (`sftpconn.cpp`):
   - TCP spojení → **SSH handshake** (libssh2 + OpenSSL).
   - **Ověření host key** proti `known_hosts`; neznámý/změněný → dotaz uživatele (callback do `sftpglue`).
   - **Autentizace**: klíč (vč. `.ppk`) → heslo → keyboard-interactive (dle nabídky serveru).
   - SFTP subsystém, nebo (SCP / fallback) shell režim.
4. FS operace volají metody `CSftpConnection` přes `sftpglue` helpery.

### Načítání knihoven
`sftp.spl` má `libssh2.dll` i `libcrypto-3-x64.dll` jako **delay-load**. Při startu `GlobalInit` nejdřív explicitně načte **naše** DLL z adresáře pluginu (`LoadLibraryEx` + `LOAD_WITH_ALTERED_SEARCH_PATH`), aby se nepoužila cizí verze z `PATH`.

---

## Sestavení

**Požadavky:** Visual Studio Build Tools 2022 (x64), Open Salamander SDK (tento plugin je součástí stromu `salamander/src/plugins/sftp`), libssh2 + OpenSSL přes vcpkg.

Nejdřív nainstaluj závislosti přes centrální build skript:

```powershell
.\tools\vcpkg\build-third-party-libs.ps1 -SftpPlugin
```

Tento příkaz:
1. naklonuje a bootstrapne vcpkg (pokud ještě není),
2. nainstaluje `libssh2` + `openssl` (3.x) do `build/vcpkg_installed_sftp/`,
3. zkopíruje DLL pro UnRAR a FTP plugin do `build/libs/` (původní funkce).

Pak sestav plugin:

```powershell
MSBuild src\vcxproj\sftp.vcxproj /p:Configuration=Release /p:Platform=x64
```

Výstup: `sftp.spl` + `lang\english.slg`. Ostatní jazyky se generují kopií (viz `bin/lang/`).

---

## Nasazení (`bin/`)

Do `plugins\sftp\` instalace Salamandera zkopírovat:
- `sftp.spl`
- `lang\*.slg` (11 jazyků)
- `libssh2.dll`, `libcrypto-3-x64.dll`, `libssl-3-x64.dll`, `z.dll`, `legacy.dll`

---

## Testování (`test/`)

Lokální testovací SSH servery v Pythonu (paramiko) + testovací klienti:

| Soubor | Účel |
|--------|------|
| `sftp_test_server.py` | SFTP server (`127.0.0.1:2222`, `test`/`test`). Env `SFTP_KBI_ONLY=1` = jen keyboard-interactive; `SFTP_MODERN_ONLY=1` = jen moderní šifry; `SFTP_PORT` = jiný port. |
| `sftp_scp_test_server.py` | Server emulující **SCP** režim (shell příkazy + scp protokol), `127.0.0.1:2223`. |
| `gen_ppk.py` | Vygeneruje testovací PuTTY `.ppk` z OpenSSH klíče. |
| `test_conn2.cpp` | Test SFTP (connect, list, upload, chmod, stat, exec, security info, klíč). |
| `test_scp.cpp` | Test SCP režimu (list, upload, download, mkdir, rename, …). |
| `test_features.cpp` | Test host key (known_hosts), resume přenosu, `.ppk` klíč. |
| `test_kbi.cpp` | Test keyboard-interactive autentizace. |

Ověřeno: SFTP i SCP plně funkční; moderní šifry (curve25519/ed25519/chacha20) proti modern-only serveru; host key save+match; resume byte-exact; `.ppk` (RSA); KBI.

---

## Co (zatím) nedělá oproti původnímu WinSCP doplňku
Záměrně **SSH-only** (SFTP+SCP). Není: FTP/FTPS, WebDAV, S3, proxy, omezení rychlosti, synchronizace adresářů, fronta na pozadí, složky uložených spojení, konverze EOL/časové zóny. SSH verze je fixně 2 (libssh2 SSH-1 neumí).
