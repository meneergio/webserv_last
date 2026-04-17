Hier is een complete `CGI_TESTING.md` die je kunt toevoegen aan je repository. Dit bestand legt precies uit hoe je de server test met de officiële tools en `curl`, zodat je dit tijdens de evaluatie als referentie kunt gebruiken.

---

# 🚀 CGI Testing Guide (Official 42 Tester)

Dit document beschrijft hoe de CGI-implementatie van deze `webserv` getest kan worden met de officiële 42 binaries (`cgi_tester`).

## 1. Voorbereiding
De `cgi_tester` binary moet aanwezig zijn in de `cgi-bin` map en uitvoerbaar zijn.

```bash
# Zorg dat de binary in de juiste map staat
cp cgi_tester www/cgi-bin/
chmod +x www/cgi-bin/cgi_tester

# Maak een symlink voor de .bla extensie (sommige testers vereisen dit)
cd www/cgi-bin && ln -sf cgi_tester cgi_tester.bla
```

## 2. Configuratie (`default.conf`)
Zorg dat de `.bla` extensie is gekoppeld aan de `cgi_tester` in de configuratie:

```nginx
location /cgi-bin {
    root    ./www/cgi-bin;
    methods GET POST;
    cgi     .bla ./www/cgi-bin/cgi_tester;
}
```

---

## 3. Testen met `curl`

### A. GET Methode (Basic Check)
Dit controleert of de server de CGI-headers correct verwerkt en een statuscode teruggeeft.
* **Commando:**
  ```bash
  curl -i http://127.0.0.1:8080/cgi-bin/cgi_tester.bla
  ```
* **Verwacht resultaat:** `HTTP/1.1 200 OK` met `Content-Type: text/html`.

### B. POST Methode (Data Processing)
De `cgi_tester` leest data van `stdin`, zet het om naar hoofdletters en stuurt het terug naar `stdout`.
* **Commando:**
  ```bash
  curl -i -X POST -d "HalloWebserv" http://127.0.0.1:8080/cgi-bin/cgi_tester.bla
  ```
* **Verwacht resultaat:**
  * Status: `200 OK`
  * Body: `HALLOWEBSERV`



---

## 4. Wat deze tests bewijzen
Tijdens de evaluatie tonen deze tests aan dat de volgende onderdelen correct werken:

1.  **Environment Variables**: De server geeft `CONTENT_LENGTH`, `REQUEST_METHOD` en `PATH_INFO` correct door via de `execve` environment.
2.  **Piping (stdin/stdout)**:
    * De server schrijft de request body succesvol naar de `stdin` van het CGI-proces.
    * De server leest de output van de CGI succesvol uit de `stdout` pipe.
3.  **Non-blocking I/O**: De `read` en `write` operaties op de CGI-pipes worden afgehandeld via de event loop (`epoll`/`kqueue`), waardoor de server niet blokkeert.
4.  **Header Parsing**: De server parset de `Status:` header van de CGI-output en zet deze om naar een valide HTTP-statuslijn.

---

## 5. Troubleshooting (Evaluatie Tips)
* **500 Internal Server Error?** Check of het pad naar de `cgi_tester` in `default.conf` absoluut is of correct relatief aan de root van de server.
* **Hanging connection?** Dit gebeurt vaak als de `stdin` pipe van de CGI niet correct wordt gesloten na het schrijven van de body. Onze server sluit deze direct met `close(pipe[1])` om een EOF-signaal te sturen.
