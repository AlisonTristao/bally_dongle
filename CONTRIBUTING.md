# Como alteramos este código

Política de engenharia do firmware T-Dongle-S3. Objetivo: manter o projeto fácil de
estender por comandos novos sem virar um emaranhado de includes, e deixar explícito o que
já foi decidido de propósito (para não reabrir a mesma discussão a cada sessão).

Leia também o [README.md](README.md) para a visão geral de arquitetura e o mapa de
comandos existentes.

## 1. Princípios gerais

- **Sem abstração prematura.** Um comando novo não precisa de uma classe nova.
  Extraia componentes para delimitar responsabilidades reais e compartilhe código que
  represente a mesma regra. Semelhança visual não basta; regras duplicadas de protocolo
  ou ciclo de vida também não se justificam por evitar abstrações. Não projete extensões
  para transportes ou comandos hipotéticos.
- **Sem comentário do óbvio.** Comentário só quando explica um *porquê* não óbvio (uma
  quirk de hardware, uma decisão de segurança, um workaround de bug específico — ex.: o
  comentário sobre `toPanelColor` no LCD, ou o do `CONFIRMAR`/`sudo` sendo guarda-corpo vs.
  controle de acesso real). Nome de função e módulo já dizem o *o quê*.
- **Sem validação de cenário impossível.** Confie nos limites internos (ex.: `context()`
  já garantido não-nulo depois do `bind()`); valide só fronteira real (entrada do usuário
  via shell, payload ESP-NOW recebido).
- **Texto voltado ao usuário em português, sem acento** (mensagens de `printLine`,
  `failWithCode`/`warnWithCode`, `help -e`); **texto de `create_module`/`shell->add` (o
  help nativo do TinyShell) e comentários/identificadores em inglês**. É a convenção já em
  uso em todo o projeto — mantenha para não misturar os dois em um mesmo arquivo.
  `scripts/check_user_text.py` audita isso automaticamente (roda sozinho em
  `platformio test -e native`, ver seção abaixo) — string em inglês esquecida num
  `printLine`/`failWithCode`, string em português no lugar errado (`create_module`/`add`)
  ou acento onde não devia quebram o check. É um heurístico por palavra-chave, não um
  parser de linguagem: falso positivo pontual (ex.: palavra nova fora da lista) é esperado
  às vezes — ajuste as listas `ENGLISH_MARKERS`/`PORTUGUESE_MARKERS` no script quando isso
  acontecer, não contorne o comando novo.
- **Build limpo é obrigatório antes de considerar uma mudança de código pronta**:
  ```bash
  platformio run -e tdongle-s3
  platformio test -e native
  ```
  Zero erros e zero warnings novos (os warnings pré-existentes do código vendorizado em
  `components/esp32-idf-sqlite3`/`components/lovyangfx` upstream, silenciados via
  `-Wno-error=*` nos `CMakeLists.txt` desses componentes, não contam). O segundo comando
  também roda `check_user_text.py` (seção 1) e os testes host-native (ver README.md § 10).
  Mudanças exclusivamente documentais exigem revisão do diff, links e coerência das
  regras, sem build de firmware.

## 2. Padrão de "módulo de comando"

Cada módulo do TinyShell (`dongle`, `espnow`, `database`, `sudo`, `help`) é uma lib própria
em `lib/<Nome>Commands/` com exatamente essa forma:

```cpp
// <Nome>Commands.cpp
namespace {
using ShellCommandSupport::context;
using ShellCommandSupport::failWithCode;
using ShellCommandSupport::printLine;
// ...

uint8_t wrapper_modulo_comando(string arg1, string arg2 = "") {
    if (context().<serviço> == nullptr) {
        return failWithCode(AppError::Code::..._NOT_READY, "mensagem em pt-br");
    }
    // ... lógica ...
    printLine("[modulo] resultado");
    return RESULT_OK;
}
} // namespace

namespace <Nome>Commands {
uint8_t registerAll() {
    context().shell->create_module("modulo", "descrição em inglês, curta");
    context().shell->add(wrapper_modulo_comando, "comando", "help text: <args>", "modulo");
    return RESULT_OK;
}
}
```

### Passo a passo para adicionar um comando novo

1. Escreva o wrapper na lib do módulo certo (`DongleCommands` para algo local ao dongle,
   `EspNowCommands` para gestão de peers/envio, `DatabaseCommands` para o SQLite, ou crie um
   módulo novo se for um domínio novo de verdade — como foi feito para `SudoCommands`).
2. Pegue os serviços via `ShellCommandSupport::context()` (`shell`, `espNow`,
   `peripherals`, `lcdDashboard`, `database`, `io`). Nunca acesse os objetos globais direto
   — sempre pelo `context()`.
3. Toda saída pro usuário via `ShellCommandSupport::printLine()` (vai pra serial + LCD +
   buffer de persistência ao mesmo tempo). Não use `Serial.print` direto num wrapper.
4. Erros: `failWithCode(AppError::Code::X, "detalhe")` retorna `RESULT_ERROR` e imprime.
   Avisos não-fatais: `warnWithCode(...)` (não muda o resultado do comando).
5. Se o comando for destrutivo (apaga dado, reseta hardware): exija
   `SudoManager::isElevated(ShellCommandSupport::currentUserId())` antes de agir — ver
   seção 5. Não reintroduza um token de confirmação em texto (`CONFIRMAR`) como única
   proteção; isso já foi removido de propósito por não proteger contra um peer remoto.
6. Registre em `registerAll()` com `context().shell->add(wrapper, "nome", "help: <args>",
   "modulo")`. Argumentos opcionais do TinyShell = parâmetro com valor default no wrapper.
7. Documente em `help -e` (`lib/HelpCommands/HelpCommands.cpp`) se for algo que um usuário
   descobriria por conta própria (não precisa documentar toda flag ali, só o essencial).
8. Atualize a tabela do módulo correspondente no `README.md`.
9. Rode o build limpo (seção 1) antes de dar por encerrado.

## 3. Arquitetura e camadas

O objetivo é ter dependências mínimas, explícitas e direcionais. **Não introduza ciclos
entre módulos**, incluindo dependências nos `.cpp`. Antes de adicionar um include,
verifique se o alvo já depende, direta ou transitivamente, da origem. Não trate uma
auditoria antiga como garantia do grafo atual. Bibliotecas não devem depender de
`AppRuntime` para descobrir serviços ou estado.

Quando duas libs em ramos diferentes do DAG (ex.: `EspNowConfig` e `EspNowCommands`)
precisam da mesma capacidade de uma lib de baixo nível (ex.: enviar bytes via
`EspNowManager`), e essa lib de baixo nível não pode saber quem está chamando: prefira um
callback de função (`BtpTransport::SendFn`) em vez de incluir a lib de topo direto — foi o
padrão usado para `BtpTransport` não incluir `EspNowManager.h` (ver README.md § 3), o que
também mantém `BtpTransport`/`ProtocolRouter` portáveis pro `env:native` (sem ESP-IDF).
Callbacks devem expor operações específicas, com contexto de execução e possibilidade
de reentrada definidos; não servem para esconder acesso ao objeto de aplicação inteiro.

Responsabilidades da composição e do shell (não é um mapa completo de comunicação):

1. **`AppRuntime`** — único dono dos objetos runtime (`EspNowManager`, `DatabaseStore`,
   `DonglePeripherals`, `LcdDashboard`, `TinyShell`, `ShellLineEditor`); monta tudo no `begin()`.
2. **`ShellConfig`** — recebe os objetos via `bind()`, registra os módulos de comando,
   implementa `runLine()` (chaining por `;`, alias, dedup de histórico, persistência).
3. **Módulos de comando** (`DongleCommands`, `EspNowCommands`, `DatabaseCommands`,
   `SudoCommands`, `HelpCommands`) — cada um só sabe do seu próprio domínio de comandos.
4. **`ShellCommandSupport`** — o *hub* intencional: agrega ponteiros pros serviços runtime
   em um `Context` só, porque as funções wrapper do TinyShell são ponteiros de função
   simples (sem forma de receber um contexto por chamada) — um contexto global é a solução
   prática pra esse formato de dispatch, não é dívida técnica a "consertar".
5. **Serviços de domínio** (`EspNowManager`, `DatabaseStore`, `DonglePeripherals`,
   `LcdDashboard`, `SudoManager`) — sem saber nada do shell.

### Quando incluir direto vs. quando passar por `Context`

`Context` (em `ShellCommandSupport.h`/`ShellConfig.h`) só guarda **ponteiros pra objetos
runtime instanciados** (`EspNowManager*`, `DatabaseStore*`, etc. — coisas que o `AppRuntime`
possui e injeta). `SudoManager` e `ShellAliases`, assim como o próprio
`ShellCommandSupport`, são **namespaces de serviço sem estado por instância** (funções
livres sobre um `static`/global interno) — está correto um módulo de comando incluir esses
direto (`#include "SudoManager.h"`) em vez de forçá-los para dentro do `Context`. Regra
prática: se o serviço é "um objeto que o `AppRuntime` cria e configura uma vez", ele entra
no `Context`; se é "um utilitário de processo, tipo uma função global", inclui direto.

Essa regra é restrita aos adaptadores do shell. Serviços de domínio recebem somente as
dependências necessárias e não acessam `ShellCommandSupport::context()`. Novos globais
mutáveis não devem ser introduzidos como atalho para passar dependências.

### Composição central e responsabilidades

`AppRuntime` permanece dono e coordenador central: monta componentes, conecta callbacks
e coordena inicialização, operação e recuperação. Pode conhecer todos os subsistemas;
cada subsistema mantém seu próprio estado, suas invariantes e detalhes de implementação.

- Métodos de coordenação mostram **quando os componentes colaboram**, em um nível
  coerente de detalhe. Implementar **como um subsistema funciona** cabe ao seu dono,
  mesmo quando esse comportamento usa várias bibliotecas.
- Descreva o propósito de cada módulo em uma frase. Nomes como `Manager`, `Config` ou
  `Support` não justificam acumular funções que mudam por motivos independentes.
- Transporte e multiplexação cuidam de bytes, enquadramento, filas e sessão. Handlers
  de comandos, manifesto e assinaturas têm responsabilidades próprias. Shell e rádio
  adaptam entradas para operações da aplicação, sem duplicar regras equivalentes.
- Use constantes, IDs e codecs canônicos do BTP; não copie offsets ou reconstrua
  formatos já implementados. Destinos de publicação devem ser considerados de maneira
  consistente por envio, contagem, taxa e limpeza, evitando listas paralelas.
- Uma operação pública deve expressar a intenção, como encerrar uma sessão. Seu dono
  garante a ordem de limpar filas, invalidar referências e liberar recursos, sem exigir
  que o chamador manipule seus campos internos.
- Separe parsing, decisão e execução quando forem etapas distintas. Sessões e
  inicialização com várias fases precisam de estados e transições definidos, incluindo
  falha, timeout, fila cheia, desconexão e repetição. Falhas parciais precisam de
  recuperação ou estado degradado explícito antes de continuar a operação.
- Não crie classes para cada grupo de funções nem interfaces genéricas sem necessidade.
  Separar um arquivo grande em vários ajuda a navegar, mas não isola estado por si só.

### Estado, concorrência e recursos

- Declare quem cria, modifica e destrói cada recurso, quais tarefas/ISRs podem acessá-lo
  e qual mecanismo protege esse acesso: fila, mutex, seção crítica ou protocolo com atomics.
  Prefira um único responsável pelas mudanças de estado quando isso simplificar o fluxo.
- Não aceite corrida "tolerada", `volatile` ou "cabe em uma palavra" como justificativa
  suficiente. Campos que formam um estado coerente precisam de proteção conjunta.
  ISRs usam mecanismos adequados ao contexto, como APIs `FromISR` ou atomics cuja
  implementação seja comprovadamente adequada à ISR; não usam mutex bloqueante.
- Referências emprestadas precisam de validade explícita. Zerar um ponteiro, torná-lo
  atômico ou conferir uma geração não mantém o objeto vivo entre a checagem e o uso.
  Desconexão e destruição devem coordenar leitores e operações em andamento.
- Use guardas RAII para mutexes adquiridos e recursos temporários: erros e retornos
  antecipados devem liberá-los automaticamente. Evite locks durante I/O demorado ou
  callbacks externos sem contrato explícito de bloqueio e ordem dos locks.
- Filas e buffers têm capacidade e comportamento em saturação definidos. Ao ampliar
  payloads, considere o custo multiplicado por filas e sessões, a stack e a margem de
  heap necessária à operação. Não reserve memória para casos hipotéticos sem necessidade.

## 4. Códigos de erro (`include/error_codes.h`)

Um range de 100 por subsistema, adicione sempre no fim do range do seu subsistema:

| Range | Subsistema |
|---|---|
| 1000–1099 | Shell / core / permissão (`SHELL_NOT_READY`, `PERMISSION_DENIED`, ...) |
| 1100–1199 | Dongle / periféricos (RTC, LCD, SD) |
| 1200–1299 | ESP-NOW |
| 1300–1399 | Database |

Ao adicionar um código: declare no `enum class Code`, adicione o `case` em `name()`. Nunca
reaproveite um número já usado (mesmo de um código removido) — histórico de logs no SD pode
ter referências antigas.

## 5. Modelo de segurança

- **`sudo` é a única barreira real para comandos destrutivos** (`dongle -sd_wipe`,
  `database -drop`, `database -rebuild`, `database -clear_logs`). A senha
  (`BoardConfig::SUDO_PASSWORD` em `include/config.h`) é uma constante compilada — trocar
  exige reflash. Elevação é por identidade (`SudoManager`), vive só em RAM, e sempre reseta
  no boot — não persista elevação em SD/banco.
- **Identidade** = string livre: `"serial"` pro console local, `"espnow:<MAC>"` por peer
  registrado. Um transporte novo (ex. MQTT) só precisa de um prefixo de identidade novo,
  sem mudar `SudoManager`.
- **Execução remota via ESP-NOW (frame BTP `COMMAND`/`COMMAND_REQUEST`, antigo `CMDO`
  pré-tópico-12)**: só roda comando de um MAC já cadastrado no registry de peers. Decisão
  deliberada: **sem restrição de conteúdo** — um peer cadastrado pode mandar qualquer
  comando, incluindo `sudo -login`. Ou seja, a defesa contra um peer malicioso é (a)
  controlar quem vira peer cadastrado e (b) a senha do `sudo`. Não adicione uma lista de
  comandos "permitidos"/"proibidos" via `COMMAND_REQUEST` sem alinhar antes — foi uma
  escolha explícita, não uma lacuna esquecida.
- Se um comando novo for destrutivo o suficiente pra merecer o mesmo tratamento, siga o
  padrão da seção 2, passo 5 — não invente um mecanismo de confirmação novo.

## 6. Git

- Só crie commits quando pedido explicitamente.
- Build limpo (seção 1) antes de qualquer commit.
- Mensagem de commit em português, curta, focada no "porquê" — mesmo estilo do histórico
  atual (`git log`).

## 7. Evolução e revisão

- Aplique estas regras incrementalmente: não amplie a dívida na área alterada e corrija
  os limites necessários à mudança. Não exija uma reescrita geral nem um limite arbitrário
  de linhas por arquivo. Tamanho é um sinal para revisão; responsabilidade é o critério.
- Em extrações, preserve comportamento e distribuição de tarefas. Mudanças de timing,
  prioridade ou protocolo devem ser explícitas e verificadas separadamente.
- Comentários descrevem contratos atuais, unidades, limites e decisões não óbvias.
  Histórico de migração pertence ao Git ou a documentos de decisões. Ao substituir um
  fluxo, revise chamadores, testes e documentação e remova os caminhos realmente obsoletos.
- Teste o comportamento afetado, inclusive conexões entre componentes: assinatura →
  geração → envio, conexão → uso → desconexão e erro → recuperação. Novos transportes
  devem passar pelos mesmos cenários compartilhados, além de seus casos específicos.
- Builds e testes nativos não comprovam timing, consumo máximo de memória ou operação
  de hardware. Quando afetados, registre a validação em placa ou o que ficou pendente.

Checklist antes de concluir uma alteração:

- Está claro onde o comportamento pertence e quais dependências utiliza?
- Foi criada dependência escondida, ciclo ou regra duplicada?
- Quem possui o estado e garante sua validade entre tarefas?
- O fluxo de falha, saturação e desconexão está definido?
- O teste verifica o comportamento necessário, incluindo a integração alterada?
- Comentários e documentação correspondem ao resultado final?
