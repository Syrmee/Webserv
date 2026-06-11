#!/bin/bash

# Couleurs pour rendre le terminal joli
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # Pas de couleur

TARGET_DIR="/tmp/www"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CGI_TESTER_SRC="$SCRIPT_DIR/cgi_tester"

echo -e "${BLUE}=== Création et configuration de $TARGET_DIR ===${NC}"

# 1. Nettoyage et création des dossiers de base
# (On recrée à neuf pour éviter les résidus des anciens tests)
rm -rf "$TARGET_DIR"
mkdir -p "$TARGET_DIR"
mkdir -p "$TARGET_DIR/upload"
mkdir -p "$TARGET_DIR/uploads" # Pour couvrir les deux variantes des fichiers de conf
mkdir -p "$TARGET_DIR/limit"
mkdir -p "$TARGET_DIR/auto"
mkdir -p "$TARGET_DIR/index_test"
mkdir -p "$TARGET_DIR/cgi-bin"
mkdir -p "$TARGET_DIR/port2"
mkdir -p "$TARGET_DIR/host2"
mkdir -p "$TARGET_DIR/YoupiBanane/nop"
mkdir -p "$TARGET_DIR/YoupiBanane/Yeah"

# 2. Fichiers HTML de base pour les tests de requêtes GET
echo "<h1>Serveur Principal de test (Port 8080)</h1>" > "$TARGET_DIR/index.html"
echo "<h1>Erreur 404 - Page personnalisee par defaut</h1>" > "$TARGET_DIR/custom_404.html"
echo "<h1>Erreur 404 - Format alternatif</h1>" > "$TARGET_DIR/404.html"
echo "<h1>Serveur de test Multi-Port (Port 8081)</h1>" > "$TARGET_DIR/port2/index.html"
echo "<h1>Serveur de test Multi-Host (Host 127.0.0.2)</h1>" > "$TARGET_DIR/host2/index.html"

# Fichier spécifique pour le test d'index de dossier
echo "<h1>Index alternatif reussi !</h1>" > "$TARGET_DIR/index_test/my_index.html"

# Fichiers bidons pour tester l'affichage de dossier (Autoindex)
echo "Ceci est un fichier texte pour tester l'autoindex." > "$TARGET_DIR/auto/fichier1.txt"
echo "Un autre fichier." > "$TARGET_DIR/auto/fichier2.html"

# 3. L'arborescence stricte exigée par le testeur de 42 (YoupiBanane)
echo "Instanciation de la structure YoupiBanane..."
echo "Contenu requis pour youpi.bad_extension" > "$TARGET_DIR/YoupiBanane/youpi.bad_extension"
echo "Fichier de test pour le CGI .bla" > "$TARGET_DIR/YoupiBanane/youpi.bla"
echo "Un fichier dans nop" > "$TARGET_DIR/YoupiBanane/nop/youpi.bad_extension"
echo "Un autre fichier dans nop" > "$TARGET_DIR/YoupiBanane/nop/other.pouic"
echo "Fichier non vide pour tester les requetes de taille" > "$TARGET_DIR/YoupiBanane/Yeah/not_happy.bad_extension"

# 4. Création du script CGI Python (pour l'évaluation humaine)
echo "Création d'un script CGI Python de test..."
cat << 'EOF' > "$TARGET_DIR/cgi-bin/test.py"
#!/usr/bin/python3
import sys

print("HTTP/1.1 200 OK")
print("Content-Type: text/html")
print("")
print("<h1>[CGI Python] Match reussi !</h1>")
EOF
chmod +x "$TARGET_DIR/cgi-bin/test.py"

# 5. Installation du binaire CGI fourni par l'école.
# Le testeur attend un executable nommé cgi_test dans /tmp/www.
echo "Installation du cgi_test officiel..."
if [ ! -f "$CGI_TESTER_SRC" ]; then
    echo "Erreur: cgi_tester introuvable: $CGI_TESTER_SRC" >&2
    exit 1
fi
cp "$CGI_TESTER_SRC" "$TARGET_DIR/cgi_test"
chmod +x "$TARGET_DIR/cgi_test"

echo -e "${GREEN}=== Arborescence créée avec succès dans $TARGET_DIR ===${NC}"
echo "Voici la structure générée :"
ls -R "$TARGET_DIR"
