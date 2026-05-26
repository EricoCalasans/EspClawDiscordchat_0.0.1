#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

// ==========================================
// Configurações centralizadas (NÃO versionar)
// ==========================================
#include "config.h"

// ==========================================
// Variáveis globais
// ==========================================
String last_msg_saas_id = "";
String last_msg_musico_id = "";
String last_msg_geral_id = "";
unsigned long boot_time = 0;

WiFiMulti wifiMulti;
WiFiClientSecure client;

// ==========================================
// FUNÇÕES AUXILIARES DO AGENTE HERMES
// ==========================================

// Função para enviar dados ao OpenRouter
String perguntarAoOpenRouter(String promptUsuario, String contextoCanal) {
  if (wifiMulti.run() != WL_CONNECTED) return "Erro: Conexão Wi-Fi perdida.";
  esp_task_wdt_reset();

  HTTPClient http;
  http.begin(client, OPENROUTER_URL);
  http.setTimeout(HTTP_TIMEOUT_MS);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(OPENROUTER_KEY));
  http.addHeader("HTTP-Referer", "https://esp32-hermes.local");
  http.addHeader("X-Title", "Hermes ESP32-S3 Agent");

  JsonDocument doc;
  doc["model"] = IA_MODEL;
  JsonArray messages = doc["messages"].to<JsonArray>();

  JsonObject msg1 = messages.add<JsonObject>();
  msg1["role"] = "system";
  msg1["content"] = "Você é o Hermes, um agente de IA autônomo rodando 24h em uma ESP32-S3 N16R8. "
                    "Você está respondendo no canal do Discord: " + contextoCanal + ". "
                    "Seja focado, inteligente e forneça respostas direto ao ponto.";

  JsonObject msg2 = messages.add<JsonObject>();
  msg2["role"] = "user";
  msg2["content"] = promptUsuario;

  String jsonString;
  serializeJson(doc, jsonString);

  int httpResponseCode = http.POST(jsonString);
  String respostaIA = "";

  if (httpResponseCode > 0) {
    String response = http.getString();
    JsonDocument respostaDoc;
    deserializeJson(respostaDoc, response);
    respostaIA = respostaDoc["choices"][0]["message"]["content"].as<String>();
  } else {
    respostaIA = "⚠️ Erro de conexão com o OpenRouter. Tente novamente.";
  }

  http.end();
  esp_task_wdt_reset();
  return respostaIA;
}

// Função para criar uma sala/canal de texto de forma autônoma no Discord
bool criarCanalDiscord(String nome_canal) {
  if (wifiMulti.run() != WL_CONNECTED) return false;
  esp_task_wdt_reset();

  HTTPClient http;
  String url = String("https://discord.com/api/v10/guilds/") + SERVER_ID + "/channels";
  http.begin(client, url);
  http.setTimeout(10000);

  http.addHeader("Authorization", "Bot " + String(DISCORD_TOKEN));
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP32-S3-Hermes-Agent (v1.0)");

  nome_canal.toLowerCase();
  nome_canal.replace(" ", "-");

  JsonDocument doc;
  doc["name"] = nome_canal;
  doc["type"] = 0;

  String jsonString;
  serializeJson(doc, jsonString);

  int httpResponseCode = http.POST(jsonString);
  http.end();

  return (httpResponseCode == 200 || httpResponseCode == 201);
}

// Função para enviar uma nova mensagem no Discord
String enviarMensagemDiscord(String canal_id, String texto) {
  if (wifiMulti.run() != WL_CONNECTED) return "";

  HTTPClient http;
  String url = String("https://discord.com/api/v10/channels/") + canal_id + "/messages";
  http.begin(client, url);
  http.setTimeout(10000);

  http.addHeader("Authorization", "Bot " + String(DISCORD_TOKEN));
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP32-S3-Hermes-Agent (v1.0)");

  JsonDocument doc;
  doc["content"] = texto;
  String jsonString;
  serializeJson(doc, jsonString);

  int httpResponseCode = http.POST(jsonString);
  String msg_id_criada = "";

  if (httpResponseCode == 200 || httpResponseCode == 201) {
    String response = http.getString();
    JsonDocument respDoc;
    deserializeJson(respDoc, response);
    msg_id_criada = respDoc["id"].as<String>();
  } else {
    Serial.printf("[Erro Discord POST] Código %d ao enviar mensagem\n", httpResponseCode);
  }
  http.end();
  return msg_id_criada;
}

// Função para editar mensagens (Sistema anti-timeout)
void editarMensagemDiscord(String canal_id, String msg_id, String novo_texto) {
  if (wifiMulti.run() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String("https://discord.com/api/v10/channels/") + canal_id + "/messages/" + msg_id;
  http.begin(client, url);
  http.setTimeout(10000);

  http.addHeader("Authorization", "Bot " + String(DISCORD_TOKEN));
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP32-S3-Hermes-Agent (v1.0)");

  JsonDocument doc;
  doc["content"] = novo_texto;
  String jsonString;
  serializeJson(doc, jsonString);

  int httpResponseCode = http.PATCH(jsonString);
  if (httpResponseCode != 200 && httpResponseCode != 201) {
    Serial.printf("[Erro Discord PATCH] Código %d ao editar mensagem\n", httpResponseCode);
  }
  http.end();
}

// Função principal de escuta de mensagens do canal
void verificarCanalDiscord(String canal_id, String nome_canal, String &last_msg_id) {
  esp_task_wdt_reset();

  HTTPClient http;
  String url = String("https://discord.com/api/v10/channels/") + canal_id + "/messages?limit=1";
  http.begin(client, url);
  http.setTimeout(10000);

  http.addHeader("Authorization", "Bot " + String(DISCORD_TOKEN));
  http.addHeader("User-Agent", "ESP32-S3-Hermes-Agent (v1.0)");

  int httpResponseCode = http.GET();

  if (httpResponseCode == 200) {
    String response = http.getString();

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
      Serial.printf("[Erro JSON] Falha ao ler canal %s\n", nome_canal.c_str());
      http.end();
      return;
    }

    if (doc.is<JsonArray>() && doc.size() > 0) {
      JsonObject msg = doc[0].as<JsonObject>();
      String msg_id = msg["id"].as<String>();
      String texto = msg["content"].as<String>();
      bool is_bot = msg["author"]["bot"].as<bool>();

      if (last_msg_id == "") {
        last_msg_id = msg_id;
        http.end();
        return;
      }

      if (msg_id != last_msg_id && !is_bot) {
        last_msg_id = msg_id;
        texto.trim();

        Serial.printf("[%s] Nova mensagem: %s\n", nome_canal.c_str(), texto.c_str());

        // COMANDO: status
        if (texto.equalsIgnoreCase("status")) {
          uint32_t uptime_segundos = (millis() - boot_time) / 1000;
          String status_str = "🟢 **[Hermes N16R8 Status]**\n"
                              "• **Uptime:** " + String(uptime_segundos) + "s\n"
                              "• **RAM Interna Livre:** " + String(ESP.getFreeHeap() / 1024) + " KB\n"
                              "• **PSRAM Livre:** " + String(ESP.getFreePsram() / (1024 * 1024)) + " MB";
          enviarMensagemDiscord(canal_id, status_str);
        }
        // COMANDO: help
        else if (texto.equalsIgnoreCase("help")) {
          String help_str = "🤖 **[Hermes Agent - Guia de Comandos]**\n"
                            "🔹 `status` - Exibe a telemetria do microcontrolador.\n"
                            "🔹 `help` - Abre este menu de ajuda.\n"
                            "🔹 `criar sala NOME` - Cria um canal de texto de forma autônoma.\n"
                            "🔹 *Qualquer outra frase* - Envia seu texto para a IA do OpenRouter.";
          enviarMensagemDiscord(canal_id, help_str);
        }
        // COMANDO: criar sala (case-insensitive)
        else {
          String textoLower = texto;
          textoLower.toLowerCase();
          if (textoLower.startsWith("criar sala ")) {
            String nomeNovaSala = texto.substring(11); // preserva capitalização original
            nomeNovaSala.trim();
            if (nomeNovaSala.length() > 0) {
              enviarMensagemDiscord(canal_id, "🛠️ *Hermes está criando a sala #" + nomeNovaSala + "...*");
              if (criarCanalDiscord(nomeNovaSala)) {
                enviarMensagemDiscord(canal_id, "✅ Sala `#" + nomeNovaSala + "` criada com sucesso!");
              } else {
                enviarMensagemDiscord(canal_id, "❌ Falha ao criar a sala. Verifique as permissões do meu Bot.");
              }
            }
          }
          // PROCESSO PADRÃO: Inteligência Artificial
          else {
            String provisoria_id = enviarMensagemDiscord(canal_id, "⏳ *Hermes está processando sua resposta...*");
            String respostaHermes = perguntarAoOpenRouter(texto, nome_canal);
            if (provisoria_id != "") {
              editarMensagemDiscord(canal_id, provisoria_id, respostaHermes);
            } else {
              enviarMensagemDiscord(canal_id, respostaHermes);
            }
          }
        }
      }
    }
  } else if (httpResponseCode == 429) {
    Serial.printf("[Rate Limit] Discord no canal %s\n", nome_canal.c_str());
  } else {
    Serial.printf("[Erro Discord GET] Código %d no canal %s\n", httpResponseCode, nome_canal.c_str());
  }
  http.end();
}

// ==========================================
// CONFIGURAÇÃO INICIAL DO SISTEMA
// ==========================================
void setup() {
  Serial.begin(115200);
  boot_time = millis();

  wifiMulti.addAP(WIFI_SSID_1, WIFI_PASS_1);
  wifiMulti.addAP(WIFI_SSID_2, WIFI_PASS_2);

  Serial.println("Hermes conectando ao sistema Wi-Fi Multi...");
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Conectado!");

  // TODO: Em produção, substituir setInsecure() por um certificado CA
  client.setInsecure();

  // Configuração segura do Watchdog para ESP32 Core v3.0+
  esp_task_wdt_config_t twdt_config = {
      .timeout_ms = WDT_TIMEOUT * 1000,
      .idle_core_mask = (1 << 0) | (1 << 1),
      .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&twdt_config);
  esp_task_wdt_add(NULL);

  Serial.println("[Hermes] Inicializado com monitoramento expandido e Task vinculada.");
}

void loop() {
  esp_task_wdt_reset();

  if (wifiMulti.run() != WL_CONNECTED) {
    delay(1000);
    return;
  }

  // Escuta sequencial dos 3 canais
  verificarCanalDiscord(CANAL_SAAS, "Ideias SaaS", last_msg_saas_id);
  delay(DELAY_ENTRE_CANAIS_MS);

  verificarCanalDiscord(CANAL_MUSICOTERAPIA, "Posts Musicoterapia", last_msg_musico_id);
  delay(DELAY_ENTRE_CANAIS_MS);

  verificarCanalDiscord(CANAL_GERAL, "Geral", last_msg_geral_id);
  delay(DELAY_ENTRE_CANAIS_MS);
}
