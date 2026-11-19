import serial, time, sys, re

PORT = "COM14"
BAUD = 115200
DURATA_S = 20

print(f"[SERIAL] Apertura {PORT}@{BAUD} DTR=True (non resetto RTS=False)...")
ser = serial.Serial(port=PORT, baudrate=BAUD, timeout=0.3,
                    dsrdtr=False, xonxoff=False, rtscts=False)
ser.dtr = True  # DTR asserito → SFP() possono stampare
ser.rts = False  # NON resettare la scheda
time.sleep(0.5)
ser.reset_input_buffer()

t0 = time.time()
raw_dump = []
while time.time() - t0 < DURATA_S:
    try:
        line = ser.readline().decode('utf-8', errors='replace').rstrip('\r\n')
    except Exception as e:
        print(f"  read error: {e}")
        continue
    if line:
        print("  |", line[:200])
        if "RAW DUMP" in line:
            raw_dump.append(line)

print(f"\n[SERIAL] Fine dopo {int(time.time()-t0)}s.")
print(f"RAW DUMP lines trovate: {len(raw_dump)}")
for l in raw_dump:
    print("\n>>", l[:500])

# Analisi: se TUTTI i 16 valori hex LEFT sono 0x00000000 → NO DATI
# Se alcuni NON zero → dati ci sono ma in posizione/ordine/endian sbagliato
cnt_zeroL = 0; cnt_nonzeroL = 0; cnt_zeroR=0; cnt_nonzeroR=0
pat = re.compile(r"L=0x([0-9A-F]{8}),R=0x([0-9A-F]{8})")
for l in raw_dump:
    for mL,mR in pat.findall(l):
        if mL == "00000000": cnt_zeroL += 1
        else: cnt_nonzeroL += 1
        if mR == "00000000": cnt_zeroR += 1
        else: cnt_nonzeroR += 1
print(f"\nAnalisi samples LEFT:  zero={cnt_zeroL}  non_zero={cnt_nonzeroL}")
print(f"Analisi samples RIGHT: zero={cnt_zeroR}  non_zero={cnt_nonzeroR}")
if cnt_nonzeroL == 0 and cnt_nonzeroR == 0:
    print("⚠️  ENTRAMBI I CANALI I2S SONO 0x00000000. GPIO11 SD non sta ricevendo nulla. (Pin SD scollegato? / VDD 3V3 mancante? / communication_format sbagliato?)")
elif cnt_nonzeroL > 0:
    print("✅ LEFT CANALE ha dati NON zero. OK pin. Problema era nella scelta canale. MIX L+R dovrebbe averli catturati.")
else:
    print("✅ RIGHT CANALE ha dati NON zero. OK pin.")

ser.close()
sys.exit(0)
