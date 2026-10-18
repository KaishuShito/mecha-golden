"""Decisions API (/v1/decisions, gpt-6-luna) でメカゴールデンの判断をする。

- presence: カメラ画像に人がいるか / こちらを向いているか (predicate)
- mood: 裏の作業 (Cockpit のタスク一覧と Airi の見回り時刻) から普段の気分を選ぶ (choice)
- addressed: マイクで拾った発言がメカゴールデンへの話しかけか (predicate + choice)
"""
import base64
import time

from openai import AsyncOpenAI

MODEL = "gpt-6-luna"
MOODS = ["idle", "working", "review", "waiting", "failed"]


class Decider:
    def __init__(self, key: str):
        self.client = AsyncOpenAI(api_key=key)
        self.calls = 0
        self.tokens = 0
        self.by_kind: dict[str, int] = {}

    async def _ask(self, kind: str, input, questions):
        t0 = time.monotonic()
        d = await self.client.decisions.create(model=MODEL, input=input, questions=questions)
        self.calls += 1
        self.by_kind[kind] = self.by_kind.get(kind, 0) + 1
        self.tokens += getattr(d.usage, "input_tokens", 0) or 0
        ans = {a.name: a for a in d.answers}
        return ans, time.monotonic() - t0

    def cost_usd(self) -> float:
        return self.tokens / 1_000_000 * 0.10

    async def presence(self, jpeg: bytes):
        img = base64.b64encode(jpeg).decode()
        ans, sec = await self._ask(
            "presence",
            [{"role": "user", "content": [
                {"type": "input_text", "text": "机の上の小さなロボット (スタックチャン) の正面カメラの画像です。カメラは少し下向きで、前に座った人は胴体だけ写ることがあります。"},
                {"type": "input_image", "image_url": "data:image/jpeg;base64," + img},
            ]}],
            [
                {"type": "predicate", "name": "person_present",
                 "instructions": "ロボットのすぐ前 (1〜2m 以内) に人がいますか。顔・胴体・腕などが大きく写っていれば true。写真・ポスター・画面の中の人や、遠くを通る人は false。"},
                {"type": "predicate", "name": "facing",
                 "instructions": "その人がロボットの方を向いていますか。顔がこちらを向いている、または体の正面をこちらに向けて座っていれば true。横向き・後ろ向き・人がいない時は false。"},
                {"type": "predicate", "name": "waving",
                 "instructions": "人がロボットに向かって手を振っていますか (手のひらを見せて左右に振る、バイバイ・やっほーの手)。手が写っていない、手を下ろしている、作業している時は false。"},
                {"type": "predicate", "name": "offering_hand",
                 "instructions": "人が手のひらを上に向けて、ロボットの前に差し出していますか (犬に「お手」と言う時の手)。手を振っている時、物を持っている時、キーボードやマウスを触っている時は false。"},
            ],
        )
        p = getattr(ans.get("person_present"), "probability", 0.0) or 0.0
        f = getattr(ans.get("facing"), "probability", 0.0) or 0.0
        self.last_gesture = (getattr(ans.get("waving"), "probability", 0.0) or 0.0, getattr(ans.get("offering_hand"), "probability", 0.0) or 0.0)
        return p, f, sec

    async def mood(self, brief: str, hb_age_min: float, now_text: str):
        text = (
            f"現在時刻: {now_text}\n"
            f"Airi (常駐 AI) の最後の見回りから {hb_age_min:.0f} 分 (30 分ごとに見回る。70 分を超えたら止まっている疑い)\n"
            "Cockpit の子タスク一覧 (id 状態 Ask有無 更新時刻(UTC) 題名):\n" + (brief.strip() or "(なし)")
        )
        ans, sec = await self._ask(
            "mood", text,
            [{"type": "choice", "name": "mood",
              "instructions": "ロボット犬が、裏の作業の様子を見て今どんな気分でいるべきかを 1 つ選んでください。状態 running はいま作業中、waiting_confirmation は作業を終えて確認待ち、ASK は人間への質問 (Ask) が出ている、error や failed は失敗です。",
              "choices": [
                  {"value": "idle", "description": "作業中のタスクも Ask も失敗もなく、静か"},
                  {"value": "working", "description": "running のタスクがあり、AI が作業している (Ask は出ていない)"},
                  {"value": "review", "description": "この 1 時間以内に作業を終えた確認待ちのタスクがあり、結果を確かめる段階"},
                  {"value": "waiting", "description": "ASK (人間への質問) が出ていて、人間の返事を待っている"},
                  {"value": "failed", "description": "error・failed のタスクがある、または Airi の見回りが 70 分以上止まっている"},
              ]}],
        )
        a = ans.get("mood")
        return getattr(a, "choice", "idle"), getattr(a, "confidence", 0.0) or 0.0, sec

    async def addressed(self, utterance: str, last_ai: str, looking: float, since_ai: float):
        text = (
            f"直前のメカゴールデンの発言 ({since_ai:.0f} 秒前): {last_ai or '(なし)'}\n"
            f"カメラ: 前の人がロボットの方を向いている確率 {looking:.2f}\n"
            f"マイクが拾った発言: {utterance}"
        )
        ans, sec = await self._ask(
            "addressed", text,
            [
                {"type": "predicate", "name": "addressed",
                 "instructions": "ロボット犬「メカゴールデン」(机の上の小さなロボット。ごはんを食べず、出かけず、仕事の作業もしない) の前でマイクが拾った発言です。この発言がメカゴールデンに向けた話しかけ・質問・呼びかけ・お願い、またはメカゴールデンの直前の質問への答えなら true。人どうしの会話 (お昼や予定の誘い、仕事の依頼や相談など、人間に向けた内容)、独り言、電話、テレビや PC の音声なら false。直前にメカゴールデンが話していても、内容が人間に向けたものなら false。"},
                {"type": "predicate", "name": "wants_end",
                 "instructions": "話している人が、メカゴールデンとの会話を終えたがっていますか (バイバイ、またね、おしまい、もういいよ、静かにして、おやすみ など)。"},
                {"type": "choice", "name": "kind",
                 "instructions": "この発言はどれにあたりますか。",
                 "choices": [
                     {"value": "to_robot", "description": "メカゴールデンへの話しかけ・返事"},
                     {"value": "to_person", "description": "ほかの人への話しかけ・人どうしの会話"},
                     {"value": "self_talk", "description": "独り言・つぶやき"},
                     {"value": "media", "description": "電話・動画・テレビ・PC などの音声"},
                     {"value": "noise", "description": "意味のある発言ではない (雑音・聞き間違い)"},
                 ]},
            ],
        )
        p = getattr(ans.get("addressed"), "probability", 0.0) or 0.0
        k = getattr(ans.get("kind"), "choice", "")
        e = getattr(ans.get("wants_end"), "probability", 0.0) or 0.0
        return p, k, e, sec

    SOUNDS = [
        ("wan", "わん: うれしい一声。軽くタップされた、呼ばれた"),
        ("wanwan", "わんわん: はしゃぐ。連打された、久しぶりに触ってもらえた、遊んでほしい"),
        ("kyan", "きゃん: びっくりした高い一声。急に触られた、しばらく誰も触らなかった後の不意打ち"),
        ("kuun", "くぅーん: さみしい・甘える。長く放っておかれた後、または元気がない時"),
        ("kuun_up", "くぅん?: なになに? と語尾が上がる。何か聞きたそう、人の返事を待っている"),
        ("hafu", "はふはふ: なでられてごきげん。前から後ろへなでられた、続けてなでてもらっている"),
        ("fuun", "ふぅーん: 満足のため息。ゆっくりなでられて落ち着いた、なで終わり"),
        ("uu", "うー: 逆なでされて、ちょっとだけ不満 (怒ってはいない)"),
    ]

    async def touch_sound(self, context: str, avoid: str = ""):
        ans, sec = await self._ask(
            "touch", context,
            [{"type": "choice", "name": "sound",
              "instructions": "ロボット犬メカゴールデン (ゴールデンレトリバーの子犬) が頭や顔を触られました。いまの触られ方と状況に、子犬としていちばん自然で可愛い鳴き声を 1 つ選んでください。同じ声ばかりにならないよう、直前に鳴いた声も参考にしてください。",
              "choices": [{"value": v, "description": d} for v, d in self.SOUNDS]}],
        )
        a = ans.get("sound")
        v = getattr(a, "choice", "wan")
        names = [n for n, _ in self.SOUNDS]
        if avoid and v == avoid:  # 同じ声が 3 回続かないよう、2 番目に合う声にする
            probs = sorted(((getattr(x, "probability", 0), getattr(x, "value", "")) for x in (getattr(a, "probabilities", None) or [])), reverse=True)
            alt = [val for _, val in probs if val != v and val in names]
            if alt:
                v = alt[0]
        return (names.index(v) if v in names else 0), getattr(a, "confidence", 0.0) or 0.0, sec
