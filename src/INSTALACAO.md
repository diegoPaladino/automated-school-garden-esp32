# Instalação e operação da horta

## Configuração de produção

- Placa: ESP32 Dev Module; porta USB usada nesta instalação: COM5.
- Dispositivo: `ESP32-HORTA-JBV-01`.
- Firmware: `v15.4.4`.
- Telemetria e irrigação automática habilitadas em `../include/config.h`.
- Credenciais locais em `../include/secrets.h`, ignorado pelo Git.
- Planilha: [Horta Automatizada - Telemetria](https://docs.google.com/spreadsheets/d/1nzFQRxhhBy7vwVbkeAVNRm4VzutjLRojM1k_jSGFWBM/edit), aba `Telemetria`.

O primeiro envio é previsto a partir de 25 segundos após a inicialização,
quando houver Wi-Fi. Depois de uma confirmação `ACK ok`, os envios ocorrem
a cada 5 minutos. Falhas são repetidas após aproximadamente 60 segundos.
Não há armazenamento das medições perdidas durante uma queda da Internet.

## Montagem física

Desligue a alimentação antes de ligar os módulos.

| Componente | Ligação configurada |
| --- | --- |
| Sensor de solo, saída analógica | GPIO36 |
| DHT11, sinal | GPIO15 |
| Relé da válvula, IN2 | GPIO23; HIGH fecha, LOW abre |
| Buzzer | GPIO13 |
| LCD 16x2 I2C, SDA / SCL | GPIO21 / GPIO22; endereço 0x3F |

Confira também a alimentação de cada módulo e o GND comum. Use a fonte
apropriada para a válvula; a conexão USB alimenta a placa e não confirma
que a instalação hidráulica está alimentada ou funcionando.

## Calibração e teste no local

1. Monte os sensores e confira `soil=OK` e `dht=OK` no monitor serial a
   115200 baud. Leituras com sensores desconectados não validam a instalação.
2. Confira a calibração gravada para a sonda usada. Para recalibrar, com a
   placa já ligada, segure BOOT por 3 a menos de 8 segundos. Um clique mede
   a referência seca; outro mede a referência úmida. Revise os valores no LCD
   e segure BOOT para salvar. O procedimento mantém a válvula fechada.
3. Com água e alimentação da válvula disponíveis, segure BOOT por pelo
   menos 8 segundos para armar o teste. Solte e clique uma segunda vez em
   até 10 segundos: a válvula abre por no máximo 3 segundos. Confirme
   fisicamente que a água entra e para. Qualquer BOOT durante o teste fecha
   a válvula. Não segure BOOT durante reset ou ao ligar a placa.
4. Confira novos registros da placa na planilha, inclusive um segundo envio
   cerca de 5 minutos depois. `soil_percent` e dados do DHT ficam vazios
   quando inválidos; os estados de falha são enviados normalmente.
5. Reinicie com a montagem pronta e confirme a recuperação do Wi-Fi,
   da calibração e dos envios antes de deixar o sistema sozinho.

## Regras atuais de irrigação

Com sensor válido e calibrado, o ciclo começa abaixo de 30% de umidade
relativa e busca 45%. Cada abertura dura 20 segundos, seguida de 90 segundos
de espera; há no máximo 3 pulsos por ciclo e intervalo de 30 minutos entre
ciclos concluídos ou interrompidos por leitura inválida. A vazão real precisa
ser observada no local para conferir se esses tempos atendem aos canteiros.

Com horário sincronizado, novos ciclos são permitidos das 06h às 22h59.
Sem horário, o código permite operação local. Uma previsão válida bloqueia
novos ciclos quando a probabilidade de chuva é pelo menos 70% e o total
previsto nas próximas 6 horas é pelo menos 2 mm. Sem previsão disponível,
a decisão continua baseada no solo. Falha do DHT não bloqueia a irrigação.

Uma leitura inválida do solo interrompe o ciclo automático. Consultas de
previsão e envio à nuvem são adiados durante o aviso de irrigação, a abertura
da válvula, a calibração e o teste manual.

## Compilação e gravação

Na pasta que contém `platformio.ini`:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e esp32dev
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e esp32dev -t upload --upload-port COM5
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor -p COM5 -b 115200
```

A gravação normal preserva a calibração em NVS. Feche o monitor serial antes
de gravar. O firmware de diagnóstico é separado e não deve substituir o de
produção para operação durante o fim de semana.

Nesta placa, a gravação exigiu segurar BOOT durante a conexão do gravador.
Solte BOOT quando aparecer `Writing at ...`. Se o programa não iniciar após
a gravação, com BOOT solto, pressione EN uma vez para reiniciar.

## Manutenção do HTTPS

O firmware inclui as raízes públicas GTS R1 e GTS R4, obtidas no
[repositório oficial Google Trust Services](https://pki.goog/repository/).
Na verificação de 29/10/2026, R1 isolada falhou e o conjunto R1 + R4 validou
`script.google.com` e `script.googleusercontent.com` pelo computador.
Os certificados continuam sendo verificados pelo firmware; uma futura
mudança da cadeia do servidor pode exigir atualização das raízes.

## Reteste do backend em 29/10/2026

Após a correção de armazenamento informada pelo responsável, a placa
recebeu `[CLOUD] ACK ok` e passou a `cloud=OK`. Foram confirmados dois registros
para `ESP32-HORTA-JBV-01`, firmware `v15.4.4`, na aba `Telemetria`:

| Linha | Horário (America/Sao_Paulo) | Uptime |
| --- | --- | --- |
| 103 | 21:56:41 | 866 segundos |
| 104 | 22:01:41 | 1.166 segundos |

O intervalo foi de exatamente 5 minutos, com avanço de 300 segundos no uptime.
O monitor serial confirmou o segundo `ACK ok`, sem observar reinicialização
nem abertura da válvula durante essa etapa. A validação do envio está concluída.

Nesse teste, os sensores estavam desconectados: solo e DHT foram registrados
como `FAULT`, sem porcentagem de solo nem valores locais de temperatura e
umidade inventados. A válvula permaneceu fechada. Isso valida o caminho de
envio e registro; a montagem, calibração física e irrigação com água ainda
precisam ser verificadas no local.

## Diagnóstico anterior do backend

A versão `v15.4.4` foi gravada com sucesso na COM5. A calibração NVS foi
preservada. A placa conectou ao Wi-Fi e recebeu HTTP 302 no POST e HTTP 200
na consulta da resposta. Antes da correção de armazenamento, o JSON do
backend retornou `ok: false` com:

```text
Exception: Você não tem permissão para acessar o documento solicitado.
```

O teste pelo computador, sem medições e identificado como diagnóstico,
recebeu o mesmo erro. Naquela etapa, a comunicação HTTPS já estava
estabelecida, mas o registro dos dados ainda não tinha sido validado.

Caso esse erro volte, para verificar o backend existente sem substituí-lo:

1. Abra o projeto Apps Script usado pela URL em `../include/secrets.h`.
   Se estiver vinculado à planilha, use Extensões → Apps Script.
2. Confira o ID usado para abrir a planilha, inclusive nas propriedades do
   script. A planilha identificada é `1nzFQRxhhBy7vwVbkeAVNRm4VzutjLRojM1k_jSGFWBM`.
3. Adicione [verificar_acesso_horta.gs](diagnostics/verificar_acesso_horta.gs)
   como arquivo adicional e execute `verificarAcessoPlanilhaHorta` no editor
   com a conta que implanta o aplicativo. O diagnóstico apenas lê a planilha.
   Conclua a autorização de acesso, caso o Google a solicite.
4. Se o diagnóstico não conseguir abrir a planilha, confira o ID e o acesso
   da conta que executa o backend. Se funcionar, confira em Implantar →
   Gerenciar implantações a identidade de execução da implantação publicada.
   Para a placa, a implantação deve executar com a conta autorizada a editar
   essa planilha e permitir chamadas sem login, mantendo a autenticação por
   token já usada no backend.
5. Se corrigir código ou propriedades da implantação, publique uma nova
   versão na implantação existente, preservando a URL quando possível.
   Caso a URL mude, atualize `GOOGLE_SCRIPT_URL` e regrave a placa.
6. Aguarde a próxima tentativa automática, prevista a cada 60 segundos
   enquanto houver falha. Confirme `[CLOUD] ACK ok`, `cloud=OK` e novas
   linhas na planilha antes de considerar o envio funcionando.

As opções de identidade e permissão de execução estão descritas na
[documentação oficial de aplicativos web do Apps Script](https://developers.google.com/apps-script/guides/web).
