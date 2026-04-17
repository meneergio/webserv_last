#!/usr/bin/env python3
import os

print("Content-Type: text/html")
print("")
print("<html><body>")
print("<h1>CGI werkt!</h1>")
print("<p>Method: " + os.environ.get("REQUEST_METHOD", "?") + "</p>")
print("<p>Query: " + os.environ.get("QUERY_STRING", "") + "</p>")
print("</body></html>")
