# piHPSDR-IF — guia do utilizador (TS-570D)

Como operar o piHPSDR-IF com o Kenwood TS-570D. Tudo o que não está aqui
funciona como no piHPSDR. Os nomes dos menus e botões estão em inglês, tal
como aparecem no programa. Instalação: `docs/IFPAN-INSTALL-LINUX.md`,
`docs/IFPAN-INSTALL-WINDOWS.md` (em inglês). Visão geral:
`docs/README-IFPAN.md`. Versão inglesa deste guia: `docs/IFPAN-USER-GUIDE.md`.

## 1. A ideia num parágrafo

O SDR não recebe a antena: recebe a **1.ª FI (73,05 MHz)** do TS-570D
através de um buffer de FI. Por isso o espectro é sempre "o que o Kenwood
vê" à volta da sua própria frequência, e o centro do ecrã é o VFO do
Kenwood (lido por CAT). O piHPSDR-IF desmodula essa FI com o WDSP (filtros
próprios, NR, AGC, RADE...), controla o Kenwood por CAT (frequência, modo,
PTT, potência, pré-amplificador, atenuador, sintonizador de antena,
altifalante) e envia o áudio de TX já processado para a entrada ACC2 do
Kenwood. O SDR nunca transmite.

Consequência: **quando o Kenwood muda de frequência, a FI inteira mexe-se.**
Rodar o botão do TS desloca sempre o espectro; nenhuma opção evita isso.

## 2. Menu Radio — bloco "Kenwood / IF panadapter"

| Controlo | O que faz |
|---|---|
| **Follow Kenwood** | Ligado: o recetor fica sempre na frequência do Kenwood (rodar o botão do TS sintoniza também o piHPSDR-IF). Desligado: quando se roda o botão do TS, o recetor mantém a estação em que estava (desvio CTUN). |
| **Clicks tune the TS** | Ligado: cada passo de sintonia feito no piHPSDR-IF (roda do rato, cliques, VFO MIDI, teclado numérico) é enviado ao Kenwood; o ecrã desliza logo (secção 4). Desligado: o espectro fica parado e só o recetor se desloca dentro da janela; o Kenwood fica onde está ("TS here" leva-o para lá). |
| **Sync mode with TS** | Modo igual nos dois sentidos (Kenwood ↔ piHPSDR-IF). DIGU / DIGL (RADE, FT8...) vão para o Kenwood como USB / LSB (áudio pela ACC2). |
| **Mute when TS in TX** | Silencia o áudio de RX enquanto o Kenwood transmite. |
| **SDR LNA off** | Mantém desligado o LNA de entrada do SDR (MSi001: +24 dB, que a libmirisdr liga acima de cerca de RF 80). O sinal do buffer de FI é forte; com o LNA o clone satura. O slider RF passa a atuar nos outros andares. |
| **SDR LO offs. (kHz)** | 0..250 kHz. Desvia o SDR da FI para que o pico DC e a imagem fiquem fora do ecrã. Clone MSi2500: **250 kHz com janela de 384k** põe ambos fora de vista (a 768k voltam, mais fracos). |
| **IF offs. \<modo\> (Hz)** | Por modo do Kenwood (aparece o do modo atual): o TS-570D desloca a portadora dentro da FI conforme o modo. Acerta-se uma vez por modo com uma estação conhecida. Já encontrado: **LSB = −3000 Hz**, USB 0. CW / CW-R / AM / FM ainda por verificar. |
| **TX audio to TS** | Saída da placa de som que alimenta a ACC2 do Kenwood (só no servidor). |
| **CAT** | "Kenwood TCP" (endereço `127.0.0.1:7373`, a ponte socat / ESP32) ou "via rigctld" (`127.0.0.1:4532`). Só no servidor; o cliente não tem ligação CAT própria. |

Outras definições: a frequência da FI é `ifpan.if_freq` no ficheiro de
props (por defeito 73050000); a calibração de frequência (ppm) no menu
Radio aplica-se ao LO da FI. Os ganhos do SDR (slider RF; "RX1 Gains" por
andar) estão na coluna da direita.

## 3. Funções da toolbar (atribuem-se nos menus Toolbar / MIDI)

| Função | Botão | O que faz |
|---|---|---|
| MOX / PTT | MOX | põe o Kenwood em emissão por CAT (`TX;` / `RX;`; `RX;` também ao sair). Recusado sem CAT. |
| TS to RX freq | **TS here** | sintoniza o Kenwood na frequência em que o piHPSDR-IF está a receber (usar com "Clicks tune the TS" desligado, antes de transmitir). |
| Preamp | **PRE** | pré-amplificador do Kenwood (`PA`). Aparece na barra do VFO. |
| TS Atten On/Off | **ATT** | atenuador do Kenwood (`RA`). Aparece na barra do VFO. |
| TS Tuner On/Off | **ATU** | sintonizador de antena ligado / em passagem (`AC 10;` / `AC 00;`). |
| TS Tuner Tune | **ATU tune** | inicia um ciclo de sintonia (`AC 11;`). Linha de estado: ATU ON / THRU / TUNING. |
| TS Speaker Mute | **TS MUTE** | põe o volume (AF) do Kenwood a 0 e repõe-no (`AG`), para comparar o áudio dele com o do piHPSDR-IF sem mexer no VOL. Rodar o VOL para cima acaba o mute; o nível é reposto ao sair. "TS MUTE" a vermelho na linha de estado. |
| Tune | TUNE | tom de 1 kHz para a ACC2 com a potência "Tune power (W)" (menu TX); a potência do Kenwood é reposta no fim; CW / FSK passam a USB / LSB durante o tune. |
| Tune Drive | TUNE_DRIVE | slider da potência de tune. |
| Two-Tone | TWO_TONE | teste de dois tons com a potência do TX Drive, mesma troca de modo. |
| RADE Menu | **RADE-M** | abre o menu RADE. |

## 4. Sintonia

**Recomendado (e é também a forma de trabalhar em remoto): sintonizar a
partir do piHPSDR-IF, não do botão do TS.** Roda do rato, cliques, teclado
numérico ou um **botão de VFO MIDI** (versão Linux/Pi; por exemplo o
controlador Arduino Due com encoder ótico de 600 PPR). Definições: "Clicks
tune the TS" **ligado**, "Follow Kenwood" ligado, CTUN ligado.

O que se vê (desde 2026-10-12): o filtro fica no centro e o espectro e o
waterfall deslizam logo, como num rádio normal. O Kenwood acompanha por
CAT umas centenas de milissegundos depois; até lá fica uma faixa vazia
numa das bordas e a linha tracejada do Kenwood mostra onde o TS está de
facto. Quando ele chega, os dados reais da FI preenchem o espaço — sem
salto para trás. (Se o Kenwood não responder em 2 s, o ecrã volta a
mostrar a posição real.) Um clique no espectro durante o deslize (< 1 s)
cai com a escala deslocada.

* **Janela (span):** 96k dá o maior detalhe à volta da estação e agora
  funciona bem porque o ecrã desliza; 384k (+ zoom) para ver uma banda
  inteira. Com o clone MSi2500 ficar em ≤ 384k (LO offset 250 kHz).
* Um salto grande a 96k: o TS-570D corta o seu recetor enquanto o PLL
  tranca, por isso a parte nova da banda pode "subir de baixo" durante um
  instante; baixar a média do espectro (menu Display) encurta isso.
* A roda do rato reenvia cada frequência ao Kenwood (até 3 vezes) porque o
  TS-570D ignora comandos que chegam quando está ocupado.
* Menu Band: bandas do TS-570 (100 kHz – 60 MHz); mudar de banda envia a
  frequência e depois o modo da band-stack (~300 ms depois, confirmado por
  leitura).
* **Rodar o botão do TS** continua a funcionar; o espectro desliza com ele
  (física: o próprio LO do Kenwood desloca a FI).

## 5. Transmitir

* **Slider TX Drive = potência de saída do Kenwood** (`PC`), passos de
  5 W, 5..100 W (AM 5..25 W, guardado por modo pelo rádio). O slider
  acompanha o rádio. Drive 0 = nenhum áudio para a ACC2.
* O microfone (local, ou o do cliente remoto) passa pela cadeia TX do WDSP
  (ganho de micro, EQ, leveler, CFC, compressor, filtro TX) e o áudio
  processado vai para a ACC2. AM / FM: processados como áudio DSB, é o
  Kenwood que faz o AM / FM. Filtro TX à volta de 200–2800 Hz para o
  filtro de 2,7 kHz do TS.
* **Redução de ruído do micro**: menu TX, página DEXP, "Mic Noise
  Reduction (NR3)" (RNNoise, antes do VOX). O VOX funciona, também a
  partir de um cliente.
* **Proteção de SWR**: menu TX "SWR alarm at" / "SWR Protection": duas
  leituras de SWR do Kenwood iguais ou acima do alarme → o Kenwood passa
  logo a RX por CAT, MOX desligado, drive a 0 (não atua enquanto o ATU
  sintoniza; em TUNE só ao fim de 10 s).
* Medidores: potência / SWR / ALC do Kenwood lidos por CAT, no medidor de
  TX do piHPSDR.
* **Ainda não testado no ar** (faltava a ficha da ACC2): potência de
  TUNE / TwoTone, nível de ALC, CW → USB durante o TUNE, desvio em FM,
  proteção de SWR em TUNE, RADE TX.
* Do lado do TS-570D para áudio pela ACC2: processador de voz desligado,
  EQ de TX plano, ALC baixo; o nível acerta-se com o volume de saída da
  placa de som.

## 6. RADE (FreeDV RADE V1)

Corre no servidor; o menu RADE do cliente (RADE-M) comanda-o à distância.
Escolher RADE põe o recetor em DIGU / DIGL (Kenwood em USB / LSB) com o
filtro certo, também no cliente. RX confirmado no ar (SYNC a ~21 dB de
SNR, indicativos descodificados; sinais abaixo de cerca de +3,8 dB de SNR
não chegam a descodificar o indicativo). Indicativo próprio: menu RADE
"My callsign". O VOX não funciona com RADE — usar PTT / MOX. Detalhes:
`docs/RADE-IFPAN.md` (em inglês).

## 7. Servidor e cliente

* O **servidor** fica junto ao rádio (agora o Raspberry Pi 5): SDR, CAT,
  placa de som da ACC2, motor RADE.
* O **cliente** (Windows ou Linux) espelha tudo: estado do Kenwood, todas
  as opções do painel de FI, TS MUTE, PRE / ATT / ATU, menu RADE,
  potência, PTT. O microfone do cliente vai para o servidor e o áudio de
  RX volta. Latência na linha de estado (0–1 ms numa rede local).
* Os dois têm de correr **a mesma versão**.
* O áudio de RX vai por UDP: um pacote perdido é um corte audível (cortes
  de ~0,5 s = perda de pacotes na rede: verificar cabos / switch,
  `ifpan-tools/net/`).
* Opção do servidor "Server runs protocol only if client is connected":
  desligar se também se usa o ecrã do próprio servidor (senão fica preto
  sem cliente).

## 8. Notas sobre os SDR

| SDR | Notas |
|---|---|
| **Clone MSi2500 / SDRplay RSP1A** (recomendado) | SoapyMiri, 1,536 MS/s isócrono, 14 bits; áudio "igual ao Hermes-Lite 2". LO offset 250 kHz a 384k, SDR LNA off, porta USB direta. No Windows o driver é libusbK. No PC Windows: bloqueio por laço de massa a ≥ 25 W (guia Windows, A6); no Pi não acontece. |
| RTL-SDR R820T (TCXO) | 1,536 MS/s, WinUSB no Windows. Funcionou bem; menos gama dinâmica. |
| PlutoSDR | primeiro SDR usado; mais fraco a 73 MHz. |

O SDR é reaberto automaticamente quando cai do USB (~1,5–2 s; depois de um
bloqueio durante TX, logo que o Kenwood volta a RX).

## 9. Logs e diagnóstico

* Log: Windows `%LOCALAPPDATA%\piHPSDR-IF\pihpsdr.stdout`; Pi
  `~/pihpsdr-if.log`.
* Linhas `cat_thread:`: estado do CAT, PTT, modo, sintonizador, TS MUTE.
* `audio diag:` (3.1.177+): falhas de envio no servidor, falhas de chegada
  no cliente, buffer de saída vazio / a transbordar.
* `mic diag:` (a cada 5 s, Windows): callbacks / amostras do micro /
  batimentos do SDR.
* Um ficheiro vazio `ifpan-txdump` na pasta de definições, no arranque do
  servidor, grava `txdump_mic.f32` / `txdump_out.f32` (30 s, float32,
  48 kHz) enquanto se transmite.
