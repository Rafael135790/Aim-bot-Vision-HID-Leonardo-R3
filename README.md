# Realtime Vision Pipeline with Arduino Leonardo R3 HID Bridge

Projeto experimental de **visão computacional em tempo real** integrado a um **Arduino Leonardo R3** para transporte de comandos via **HID/RawHID**, com foco em:

- baixa latência;
- processamento em janela reduzida;
- tolerância a falhas de captura e reconexão;
- agregação de entrada USB no microcontrolador;
- integração entre software Python e firmware embarcado.

O sistema é composto por três camadas técnicas:

1. **Aplicação Python**
   - captura de tela com DXCam;
   - processamento de imagem com OpenCV/NumPy;
   - seleção geométrica de referência;
   - geração de deltas incrementais;
   - envio de comandos para o microcontrolador por HID.

2. **Firmware para Arduino Leonardo R3**
   - recepção de relatórios RawHID;
   - uso do USB nativo do ATmega32U4;
   - agregação de deltas vindos do Python com deltas vindos de um dispositivo USB externo conectado via Host Shield;
   - emissão final de eventos HID para o computador host.

3. **Modificações no core USB/HID**
   - ajustes no stack USB para operação sem CDC;
   - adaptação da configuração de endpoints;
   - alterações voltadas à estabilidade e ao layout de interface desejado;
   - derivadas do ecossistema open source Arduino, com preservação de créditos e licenças.

---

## Visão geral da arquitetura

O projeto foi desenvolvido para demonstrar uma cadeia completa de processamento em tempo real:

```text
Captura de tela -> Segmentação de imagem -> Extração de referência -> Geração de deltas ->
Empacotamento HID -> Arduino Leonardo R3 -> Consolidação de entrada -> HID final ao host
```

```text
+--------------------------------------------------------------+
|                        Computador Host                       |
|                                                              |
|  +-------------------+      +----------------------------+   |
|  | DXCam             | ---> | OpenCV + NumPy            |   |
|  | Captura central   |      | HSV + máscara circular    |   |
|  +-------------------+      +-------------+-------------+   |
|                                            |                 |
|                                            v                 |
|                                +--------------------------+  |
|                                | NeuroMotor               |  |
|                                | Perfil temporal          |  |
|                                | Acumuladores float       |  |
|                                +-------------+------------+  |
|                                              |               |
|                                              v               |
|                                +--------------------------+  |
|                                | HID / hidapi            |   |
|                                | Relatório de 64 bytes   |   |
|                                +-------------+------------+  |
+----------------------------------------------|---------------+
                                               |
                                               v
+--------------------------------------------------------------+
|                   Arduino Leonardo R3 (ATmega32U4)           |
|                                                              |
|  +-------------------+      +----------------------------+   |
|  | RawHID receive    | ---> | Acúmulo de deltas         |   |
|  | 64 bytes          |      | deltaPy / remainder       |   |
|  +-------------------+      +----------------------------+   |
|                                                              |
|  +-------------------+      +----------------------------+   |
|  | USB Host Shield   | ---> | HID parser                |   |
|  | Dispositivo USB   |      | deltaMouse / wheel        |   |
|  +-------------------+      +----------------------------+   |
|                                                              |
|                     +---------------------------+            |
|                     | Mouse.move() HID nativo   |            |
|                     +---------------------------+            |
+--------------------------------------------------------------+
```

---

## Por que o Arduino Leonardo R3 é central neste projeto

O **Arduino Leonardo R3** usa o **ATmega32U4**, que possui **USB nativo**. Isso permite que a placa se apresente diretamente ao host como **dispositivo HID**, sem depender de uma interface USB-serial separada.

### Vantagens práticas do Leonardo R3

- pode se apresentar diretamente ao host como **mouse/HID**;
- pode usar bibliotecas como `HID-Project` e `RawHID`;
- permite controle do comportamento USB com muito mais liberdade;
- possibilita alterações no core USB para ajustar descriptors, endpoints e interfaces;
- elimina a dependência de uma camada serial convencional para o transporte principal.

### Consequência arquitetural

Neste projeto, o Leonardo R3 atua como:

- **receptor RawHID** para dados vindos da aplicação Python;
- **ponte HID** para o host;
- **agregador de entrada**, somando deltas locais e externos;
- **nó embarcado de real-time I/O**, com watchdog e recuperação do barramento USB host.

---

## Estrutura do projeto

```text
realtime-vision-pipeline-leonardo-r3/
├─ README.md
├─ LICENSE
├─ .gitignore
├─ python/
│  └─ vision_hid_bridge.py
├─ arduino/
│  └─ leonardo_r3_hid_bridge/
│     └─ leonardo_r3_hid_bridge.ino
├─ core_patches/
│  ├─ USBAPI_modified.cpp
│  └─ USBDesc_modified.h
└─ docs/
   ├─ architecture.md
   ├─ protocol.md
   ├─ leonardo-r3-notes.md
   └─ implementation-details.md
```

---

## Camada Python

A aplicação Python é responsável por:

1. localizar a interface HID correta por `vendor_id`, `product_id` e `interface_number`;
2. iniciar a captura de tela com DXCam;
3. processar apenas uma região pequena da tela (`FOV = 12`);
4. converter o frame para HSV;
5. aplicar segmentação por faixa de cor;
6. aplicar uma máscara circular para ignorar os cantos da janela;
7. localizar o pixel mais alto da região detectada;
8. transformar esse ponto em um erro relativo ao centro;
9. passar esse erro para a classe `NeuroMotor`;
10. empacotar os deltas gerados e enviá-los via HID ao Arduino Leonardo R3.

### Pipeline de visão

A lógica foi desenhada para minimizar custo computacional:

- o FOV é pequeno;
- a captura ocorre apenas na região central;
- a máscara reduz o número de pixels relevantes;
- a seleção do ponto superior evita rastreamento complexo;
- o envio HID é feito por pacotes curtos e em alta frequência.

### Estabilidade

A aplicação inclui mecanismos de tolerância a falhas:

- reconexão automática do dispositivo HID;
- watchdog da câmera baseado em timestamp de frame;
- reinício do DXCam quando não há frames novos por um intervalo configurável;
- tratamento de exceções no loop principal;
- encerramento seguro ao finalizar o processo.

---

## Classe `NeuroMotor`

A classe `NeuroMotor` implementa uma camada de suavização e modelagem temporal do movimento.

Ela não envia diretamente a posição alvo; em vez disso, converte o erro espacial `(dx, dy)` em deltas progressivos ao longo do tempo.

### Funções principais

- manutenção de histórico recente;
- geração de tendência local;
- introdução de drift controlado;
- cálculo de duração planejada do movimento;
- fase de reação antes do início da correção;
- acumulação fracionária (`acc_x`, `acc_y`) para reduzir perdas por truncamento.

### Interpretação técnica

Ao detectar um novo alvo, o método `update()` define:

- o instante de início;
- a janela de reação;
- a duração do movimento;
- a decisão de atuar ou ignorar.

Depois disso, o progresso do movimento é modelado por um parâmetro normalizado `tau`, o que faz com que os comandos não sejam emitidos em um único salto, mas distribuídos ao longo do tempo em pequenos incrementos. Isso reduz serrilhado e melhora a estabilidade da resposta discreta.

---

## Firmware do Arduino Leonardo R3

O sketch para o Leonardo R3 faz a ponte entre duas entradas distintas:

1. **entrada USB externa via Host Shield**;
2. **entrada RawHID vinda do Python**.

Essas duas fontes são consolidadas em uma única saída HID apresentada ao host.

### Responsabilidades do firmware

- inicializar o host USB;
- processar relatórios HID do dispositivo conectado;
- armazenar deltas de mouse e wheel;
- receber relatórios RawHID de 64 bytes vindos do Python;
- acumular deltas de ambas as origens;
- aplicar persistência temporal para habilitação condicional;
- emitir `Mouse.move()` com os dados consolidados.

### Pontos importantes do ATmega32U4

O ATmega32U4 é especialmente adequado aqui porque oferece:

- USB full-speed nativo;
- controle direto da pilha USB do dispositivo;
- suporte a bibliotecas HID;
- possibilidade de alterar descriptors e endpoints.

### Robustez embarcada

O firmware possui mecanismos explícitos de resiliência:

- `watchdog timer` habilitado;
- timeout de atividade do USB host;
- tentativa de recuperação do barramento com `Usb.Init()`;
- descarte de pacote RawHID incompleto após timeout;
- acúmulo com `remainderX` e `remainderY` para preservar frações.

---

## Protocolo HID / RawHID

O protocolo usa um relatório fixo de 64 bytes.

### Estrutura conceitual

```text
Byte 0  -> reservado / report id
Byte 1  -> delta X
Byte 2  -> delta Y
Byte 3  -> reservado / wheel
Bytes 4..63 -> padding
```

### Motivo do formato fixo

Mesmo quando poucos bytes carregam informação útil, o uso de pacotes fixos de 64 bytes simplifica o parsing no firmware, reduz ambiguidade de framing e facilita depuração.

---

## Modificações no core USB/HID

Além do sketch, este projeto inclui modificações em arquivos do core USB usados pelo Leonardo R3.

Essas alterações afetam diretamente:

- descriptors;
- classe do dispositivo;
- composição de interfaces;
- tamanho de endpoints;
- presença ou ausência de CDC;
- forma como o stack inicializa endpoints.

### Pontos observados

- desativação de CDC (`CDC_DISABLED`);
- descriptor mais apropriado quando CDC está desativado;
- suporte explícito a endpoint de 16 bytes;
- preservação da integração com `PluggableUSB`.

Como o Leonardo R3 expõe diretamente sua interface USB pelo ATmega32U4, essas alterações fazem parte estrutural do projeto.

---

## Como publicar este projeto no GitHub

### Onde cada arquivo entra

Na raiz do repositório:

- `README.md`
- `LICENSE`
- `.gitignore`

Dentro da pasta `docs/`:

- `architecture.md`
- `protocol.md`
- `leonardo-r3-notes.md`
- `implementation-details.md`

Dentro da pasta `python/`:

- seu código Python principal

Dentro da pasta `arduino/leonardo_r3_hid_bridge/`:

- seu sketch `.ino`

Dentro da pasta `core_patches/`:

- seus arquivos modificados do core USB/HID

---

## Requisitos

### Software

- Windows
- Python 3.10+
- Arduino IDE
- `hid`
- `dxcam`
- `opencv-python`
- `numpy`

Instalação das dependências Python:

```bash
pip install hid dxcam opencv-python numpy
```

### Hardware

- Arduino Leonardo R3
- USB Host Shield
- dispositivo USB HID externo compatível com o parser
- cabo USB confiável
- alimentação estável

---

## Aviso de licenciamento

Os arquivos em `core_patches/` são derivados de componentes open source do ecossistema Arduino e mantêm seus avisos de copyright e licenças originais.

Este repositório documenta modificações e integração sobre essas bases, e não reivindica autoria exclusiva sobre o código-fonte originalmente distribuído por esses projetos.

---

## Finalidade

Este projeto é apresentado como estudo técnico de:

- visão computacional em tempo real;
- HID e RawHID;
- USB nativo do ATmega32U4;
- integração entre software Python e firmware embarcado;
- recuperação de barramento USB e robustez de execução.
