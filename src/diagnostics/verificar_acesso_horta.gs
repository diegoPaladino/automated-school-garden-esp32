/**
 * Adicione este arquivo ao projeto Apps Script existente do backend.
 * Execute verificarAcessoPlanilhaHorta no editor, com a conta que implanta
 * o aplicativo web. Não substitui doGet/doPost e não grava dados.
 */
function verificarAcessoPlanilhaHorta() {
  const spreadsheetId = '1nzFQRxhhBy7vwVbkeAVNRm4VzutjLRojM1k_jSGFWBM';
  const spreadsheet = SpreadsheetApp.openById(spreadsheetId);
  const sheet = spreadsheet.getSheetByName('Telemetria');
  if (!sheet) {
    throw new Error('A planilha abriu, mas a aba Telemetria não foi encontrada.');
  }

  const header = sheet.getRange('A1:C1').getValues()[0];
  const expected = ['timestamp_server', 'device_id', 'firmware_version'];
  if (!expected.every(function (value, index) { return header[index] === value; })) {
    throw new Error('Os cabeçalhos não correspondem à planilha de telemetria esperada.');
  }

  const result = {
    ok: true,
    spreadsheet_id: spreadsheet.getId(),
    spreadsheet_name: spreadsheet.getName(),
    sheet: sheet.getName(),
    last_row: sheet.getLastRow()
  };
  console.log(JSON.stringify(result));
  return result;
}
