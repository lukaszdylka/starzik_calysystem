cat > Makefile << 'EOF'
master:
	pio run -e master

golab:
	pio run -e golab
	
walizka:
	pio run -e walizka

web:
	pio run -e master -t uploadfs

upload-master:
	pio run -e master -t upload

upload-golab:
	pio run -e golab -t upload

upload-walizka:
	pio run -e walizka -t upload

all: master golab walizka
EOF

# === KROK 6: TEST ===
echo "🧪 Testuję kompilację..."

make master
if [ $? -eq 0 ]; then
    echo "✅ Master OK!"
    make golab
    if [ $? -eq 0 ]; then
        echo "✅ Gołąb OK!"
        make walizka
        if [ $? -eq 0 ]; then
            echo "✅ Walizka OK!"
            echo ""
            echo "🎉 SUKCES! Wszystkie urządzenia skompilowane!"
            echo ""
            echo "📋 Następne kroki:"
            echo "make upload-master    # Upload Master"
            echo "make upload-golab     # Upload Gołąb" 
            echo "make upload-walizka   # Upload Walizka"
            echo "make web              # Upload panel www"
        fi
    fi
fi