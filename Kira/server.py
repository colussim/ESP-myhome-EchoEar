import os
import io
import uuid
import mlx_whisper
import uvicorn
import librosa
import numpy as np
import json 
import requests 
import ollama
import json
import importlib
from fastapi import FastAPI, Request, HTTPException
from dotenv import load_dotenv

# Load Initial Prompt
def load_prompt_from_file(file_name="prompt.txt"):
    if os.path.exists(file_name):
        with open(file_name, "r", encoding="utf-8") as f:
            # We clean up the line breaks to have a single string 
            return f.read().replace("\n", " ").strip()

PROMPT_MEMORY = load_prompt_from_file()

# Dictionnaire pour stocker les fonctions Python chargées
SERVICES_DYNAMIQUES = {}

def charger_capacites():
    with open("config/tools_config.json", "r", encoding="utf-8") as f:
        config = json.load(f)
    
    tools_ollama = []
    for tool_def in config["definitions"]:
        name = tool_def["function"]["name"]
        if name in config["active_tools"]:
            try:
                # Import dynamique du fichier dans services/name.py
                module = importlib.import_module(f"services.{name}")
                SERVICES_DYNAMIQUES[name] = module.run
                tools_ollama.append(tool_def)
                print(f"✅ Capacité chargée : {name}")
            except Exception as e:
                print(f"❌ Erreur sur le service {name} : {e}")
    return tools_ollama

# Au démarrage du serveur
KIRA_TOOLS = charger_capacites()


def get_kira_weather(user_text):
    # On interroge le modèle spécialisé
    response = ollama.chat(
        model='konsumer/weather',
        messages=[{'role': 'user', 'content': user_text}]
    )
    
    # Le modèle va te répondre un truc du genre : 
    # {"name": "current", "parameters": {"city": "Paris"}}
    return response.message.content


# 1. FORCE LE PATH FFMEG (Crucial sur Mac Studio)
os.environ["PATH"] += os.pathsep + "/opt/homebrew/bin"

# Time
from datetime import datetime
now = datetime.now().strftime("%d/%m/%Y %H:%M")

# Charger les tokens depuis le fichier .env
load_dotenv()
API_TOKEN = os.getenv("KIRA_API_TOKEN")
HA_TOKEN = os.getenv("HA_TOKEN")
HA_URL = os.getenv("HA_URL")
USE_OLLAMA = int(os.getenv("USE_OLLAMA", 0))
MODEL_PATH = os.getenv("MODEL_STT")
TEMPERATURE = float(os.getenv("TEMPERATURE", 0.0)) 
BEST_OF = int(os.getenv("BEST_OF", 1))
SUPPRESS_TOKENS = os.getenv("SUPPRESS_TOKENS", "-1")
CONDITION_ON_PREVIOUS_TEXT = os.getenv("CONDITION_ON_PREVIOUS_TEXT", "0") == "1"
LANGUAGE=os.getenv("LANGUAGE")
#PROMPT_MEMORY=os.getenv("INITIAL_PROMPT")

# Liste des variantes du nom pour le nettoyage et la détection d'éveil
mots_a_supprimer = ["kyra", "kira", "tyra", "tira", "lyra", "ira"]

# Liste étendue pour capturer l'éveil plus facilement
mots_eveil = ["Dikira","diskira","dis kira", "dit kira", "dis, kira", "dit, kira", "dit tira", "dit, tira", "dit kyra", "dis kyra", "dis, kyra", "dis lyra", "dit lyra", "dis, lyra", "dis ira", "dit ira", "dis, ira"]


app = FastAPI()

# LE PROMPT DE ROUTAGE (Pour Ollama)
ROUTER_PROMPT = """
Tu es le cerveau de Kira. Analyse la phrase de l'utilisateur et réponds UNIQUEMENT en JSON.
Choisis une catégorie : 
1. "HA" (Action sur la domotique)
2. "INFO" (Question de culture, météo, recherche)
3. "TALK" (Discussion simple)

Format : {"category": "...", "reason": "..."}
"""

def ask_home_assistant(text):
    url = f"{HA_URL}/conversation/process" 
    headers = {
        "Authorization": f"Bearer {HA_TOKEN}",
        "Content-Type": "application/json",
    }
    payload = {"text": text, "language": "fr"}
    try:
        r = requests.post(url, headers=headers, json=payload, timeout=5)
        if r.status_code != 200:
            print(f"DEBUG HA : Code {r.status_code} - {r.text}")
            return {"success": False, "speech": f"Erreur HA {r.status_code}"}
        res = r.json()
        speech = res.get("response", {}).get("speech", {}).get("plain", {}).get("speech", "Fait.")
        return {"success": True, "speech": speech}
    except Exception as e:
        print(f"DEBUG HA : Erreur de connexion : {e}")
        return {"success": False, "speech": "La maison ne répond pas."}

def ask_knowledge(text):
    """Demande à Mistral ou Llama d'utiliser sa culture générale"""
    try:
        response = ollama.chat(model='mistral-nemo', messages=[
            {'role': 'system', 'content': f"Tu es Kira, une IA cultivée. Nous sommes le {now}. Réponds de façon concise."},
            {'role': 'user', 'content': text},
        ])
        return response['message']['content']
    except Exception as e:
        return f"Désolée, mon cerveau est indisponible : {str(e)}"
    
@app.post("/transcribe")
async def process_kira(request: Request):
    # Sécurité Token
    if request.headers.get("X-Token") != API_TOKEN:
        raise HTTPException(status_code=403)

    audio_data = await request.body()
    try:
        # 1. ÉCOUTER (Whisper MLX)
        with io.BytesIO(audio_data) as audio_file:
            audio_array, _ = librosa.load(audio_file, sr=16000)
            
        result = mlx_whisper.transcribe(
            audio_array, 
            path_or_hf_repo=MODEL_PATH, 
            initial_prompt=PROMPT_MEMORY, 
            temperature=TEMPERATURE, 
            language=LANGUAGE, 
            best_of=BEST_OF, 
            suppress_tokens=SUPPRESS_TOKENS, 
            condition_on_previous_text=CONDITION_ON_PREVIOUS_TEXT
        )

        segments = result.get('segments', [])
        if segments:
            no_speech_prob = segments[0].get('no_speech_prob', 0)
            print(f"DEBUG : Probabilité de bruit seul : {no_speech_prob:.2f}")
            if no_speech_prob > 0.6:
                print("ERREUR : Bruit détecté, commande ignorée.")
                return {"status": "ignored", "reason": "noise"}

        user_text = result['text'].strip()
        user_text_clean = user_text.lower().replace(".", "").replace(",", "").strip()
        print(f"User: {user_text}")

        # Détection ÉVEIL SEUL
        is_wake_only = any(variant == user_text_clean for variant in mots_eveil)
        if is_wake_only:
             print(f">>> ÉVEIL CONFIRMÉ : [{user_text_clean}]")
             return {
                "status": "success", "heard": user_text, "kiraactive": True,
                "category": "WAKE", "reply": "Oui, je t'écoute ?"
             }

        # --- LOGIQUE ROUTAGE ET TOOLS (Ollama) ---
        category = "TALK"
        reply_content = ""
        ha_ack = "no_action"

        if USE_OLLAMA == 1:
            try:
                # Appel Ollama avec gestion des TOOLS dynamiques
                response = ollama.chat(
                    model='mistral-nemo', 
                    messages=[{'role': 'user', 'content': user_text}],
                    tools=KIRA_TOOLS # On injecte les outils chargés du JSON
                )

                # A. Cas où l'IA veut utiliser un TOOL (Météo, Wikipedia, etc.)
                if response.get('message', {}).get('tool_calls'):
                    category = "TOOL"
                    for call in response['message']['tool_calls']:
                        func_name = call['function']['name']
                        args = call['function']['arguments']
                        
                        if func_name in SERVICES_DYNAMIQUES:
                            print(f"🛠️ Kira utilise l'outil : {func_name}({args})")
                            resultat_tool = SERVICES_DYNAMIQUES[func_name](**args)
                            
                            # On renvoie le résultat à l'IA pour la phrase finale
                            final_resp = ollama.chat(
                                model='mistral-nemo',
                                messages=[
                                    {'role': 'user', 'content': user_text},
                                    response['message'],
                                    {'role': 'tool', 'content': str(resultat_tool), 'name': func_name}
                                ]
                            )
                            reply_content = final_resp['message']['content']
                        else:
                            reply_content = "J'ai essayé d'utiliser un outil que je ne maîtrise pas encore."
                
                # B. Pas de Tool, on utilise ton ROUTER_PROMPT classique
                else:
                    route_resp = ollama.chat(model='mistral-nemo', format='json', messages=[
                        {'role': 'system', 'content': ROUTER_PROMPT},
                        {'role': 'user', 'content': user_text},
                    ])
                    decision = json.loads(route_resp['message']['content'])
                    category = decision.get("category", "TALK")

            except Exception as e:
                print(f"Erreur intelligence Ollama: {e}")
                category = "TALK"

        # 3. EXÉCUTER les catégories classiques (HA / INFO / TALK)
        # Si reply_content est déjà rempli par un TOOL, on saute cette partie
        if not reply_content:
            if category == "HA":
                text_for_ha = user_text.lower()
                for mot in mots_a_supprimer:
                    text_for_ha = text_for_ha.replace(mot + ",", "").replace(mot, "")
                text_for_ha = text_for_ha.strip()
                
                print(f"Action : Domotique (Envoi : {text_for_ha})") 
                ha_res = ask_home_assistant(text_for_ha)
                reply_content = ha_res["speech"]
                ha_ack = "ok" if ha_res["success"] else "error"
                
            elif category == "INFO":
                print("Action : Connaissance")
                reply_content = ask_knowledge(user_text)
                ha_ack = "no_action"
                
            else: # TALK
                print("Action : Discussion")
                reply_content = ask_knowledge(user_text) if USE_OLLAMA == 1 else "Je vous écoute."
                ha_ack = "no_action"

        return {
            "status": "success",
            "heard": user_text,
            "kiraactive": False,
            "reply": reply_content,
            "ha_ack": ha_ack,
            "category": category
        }

    except Exception as e:
        print(f"Erreur globale : {e}")
        raise HTTPException(status_code=500, detail=str(e))
if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
