#!/usr/bin/env python3
"""给外研版四年级上册词表补音标。

音标手工标注（英式/美式均可，此处用国际音标近似标法），
词组按单词连读标注。输出 data/wy4a_words.csv。
"""

import csv
from pathlib import Path

ROOT = Path(__file__).parent
SRC = ROOT / "data" / "wy4a_textbook.csv"
DST = ROOT / "data" / "wy4a_words.csv"

# word -> phonetic
PHONETICS = {
    # Unit 1
    "star": "/stɑː/", "ping-pong": "/ˈpɪŋ pɒŋ/", "kick": "/kɪk/", "band": "/bænd/",
    "very": "/ˈveri/", "fast": "/fɑːst/", "sure": "/ʃʊə/", "hurt": "/hɜːt/",
    "really": "/ˈrɪəli/", "wind": "/wɪnd/", "ride": "/raɪd/", "report": "/rɪˈpɔːt/",
    "wheelchair": "/ˈwiːltʃeə/", "far": "/fɑː/", "from": "/frɒm/",
    # Unit 2
    "town": "/taʊn/", "train": "/treɪn/", "station": "/ˈsteɪʃn/",
    "train station": "/ˈtreɪn ˈsteɪʃn/", "supermarket": "/ˈsuːpəmɑːkɪt/",
    "cinema": "/ˈsɪnəmə/", "bus": "/bʌs/", "stop": "/stɒp/",
    "bus stop": "/ˈbʌs stɒp/", "hospital": "/ˈhɒspɪtl/", "police": "/pəˈliːs/",
    "police station": "/pəˈliːs ˈsteɪʃn/", "shop": "/ʃɒp/", "first": "/fɜːst/",
    "left": "/left/", "then": "/ðen/", "straight": "/streɪt/", "turn": "/tɜːn/",
    "there": "/ðeə/", "surprise": "/səˈpraɪz/", "restaurant": "/ˈrestrɒnt/",
    "excuse me": "/ɪkˈskjuːz miː/", "way": "/weɪ/", "us": "/ʌs/",
    "thanks": "/θæŋks/", "buy": "/baɪ/", "our": "/ˈaʊə/", "writer": "/ˈraɪtə/",
    "often": "/ˈɒfn/", "between": "/bɪˈtwiːn/", "street": "/striːt/",
    "live": "/lɪv/", "place": "/pleɪs/", "near": "/nɪə/", "map": "/mæp/",
    # Unit 3
    "everyone": "/ˈevriwʌn/", "work": "/wɜːk/", "hard": "/hɑːd/",
    "teacher": "/ˈtiːtʃə/", "driver": "/ˈdraɪvə/", "firefighter": "/ˈfaɪəɡaɪtə/",
    "policeman": "/pəˈliːsmən/", "worker": "/ˈwɜːkə/", "doctor": "/ˈdɒktə/",
    "nurse": "/nɜːs/", "teach": "/tiːtʃ/", "sick": "/sɪk/", "gardener": "/ˈɡɑːdnə/",
    "garden": "/ˈɡɑːdn/", "tall": "/tɔːl/", "cleaner": "/ˈkliːnə/",
    "fire": "/ˈfaɪə/", "animal": "/ˈænɪml/", "grow": "/ɡrəʊ/",
    "taxi": "/ˈtæksi/", "drive": "/draɪv/", "around": "/əˈraʊnd/",
    "farmer": "/ˈfɑːmə/", "science": "/ˈsaɪəns/", "ask": "/ɑːsk/",
    "question": "/ˈkwestʃən/", "scientist": "/ˈsaɪəntɪst/", "because": "/bɪˈkɒz/",
    "panda": "/ˈpændə/", "keeper": "/ˈkiːpə/", "house": "/haʊs/",
    "dream job": "/driːm dʒɒb/",
    # Unit 4
    "zookeeper": "/ˈzuːkiːpə/", "bee": "/biː/", "early": "/ˈɜːli/",
    "go shopping": "/ɡəʊ ˈʃɒpɪŋ/", "fifty": "/ˈfɪfti/", "yuan": "/juˈɑːn/",
    "thirty-three": "/ˌθɜːti ˈtriː/", "forty-four": "/ˌfɔːti ˈfɔː/",
    "carrot": "/ˈkærət/", "thirteen": "/ˌθɜːˈtiːn/", "seventy": "/ˈsevnti/",
    "eleven": "/ɪˈlevn/", "thirty": "/ˈθɜːti/", "eighteen": "/ˌeɪˈtiːn/",
    "eighty": "/ˈeɪti/", "nineteen": "/ˌnaɪnˈtiːn/", "sixteen": "/ˌsɪksˈtiːn/",
    "sixty-six": "/ˌsɪksti ˈsɪks/", "twenty": "/ˈtwenti/", "twelve": "/twelv/",
    "hundred": "/ˈhʌndrəd/", "forty-three": "/ˌfɔːti ˈθriː/",
    "market": "/ˈmɑːkɪt/", "flea market": "/ˈfliː ˈmɑːkɪt/",
    "skateboard": "/ˈskeɪtbɔːd/", "noon": "/nuːn/", "money": "/ˈmʌni/",
    "by the way": "/baɪ ðə weɪ/", "skateboarding": "/ˈskeɪtbɔːdɪŋ/",
    "friend": "/frend/", "flower": "/ˈflaʊə/", "toy": "/tɔɪ/",
    "pair": "/peə/", "give": "/ɡɪv/", "spend": "/spend/", "box": "/bɒks/",
    "use": "/juːz/", "save": "/seɪv/", "holiday": "/ˈhɒlədeɪ/",
    "share": "/ʃeə/", "should": "/ʃʊd/",
    # Unit 5
    "shape": "/ʃeɪp/", "chocolate": "/ˈtʃɒklət/", "triangle": "/ˈtraɪæŋɡl/",
    "circle": "/ˈsɜːkl/", "rectangle": "/ˈrektæŋɡl/", "square": "/skweə/",
    "into": "/ˈɪntuː/", "tummy": "/ˈtʌmi/", "yummy": "/ˈjʌmi/",
    "wheel": "/wiːl/", "another": "/əˈnʌðə/", "a lot": "/ə lɒt/",
    "its": "/ɪts/", "line": "/laɪn/",
    # Unit 6
    "zoo": "/zuː/", "sky": "/skaɪ/", "neck": "/nek/",
    "strong": "/strɒŋ/", "brave": "/breɪv/", "heavy": "/ˈhevi/",
    "giraffe": "/dʒəˈrɑːf/", "lion": "/ˈlaɪən/", "elephant": "/ˈelɪfənt/",
    "pig": "/pɪɡ/", "horse": "/hɔːs/", "sheep": "/ʃiːp/", "cow": "/kaʊ/",
    "climb": "/klaɪm/", "slow": "/sləʊ/", "perfect": "/ˈpɜːfɪkt/",
    "pet": "/pet/", "boy": "/bɔɪ/", "girl": "/ɡɜːl/", "hamster": "/ˈhæmstə/",
    "cute": "/kjuːt/", "parrot": "/ˈpærət/", "talk": "/tɔːk/",
    "shelter": "/ˈʃeltə/", "animal shelter": "/ˈænɪml ˈʃeltə/",
    "hurt": "/hɜːt/", "catch": "/kætʃ/", "grass": "/ɡrɑːs/",
    "farm": "/fɑːm/", "tail": "/teɪl/",
    # Plays
    "city": "/ˈsɪti/", "party": "/ˈpɑːti/", "gift": "/ɡɪft/",
    "address": "/əˈdres/", "snow": "/snəʊ/", "knock": "/nɒk/",
    "wolf": "/wʊlf/", "paw": "/pɔː/", "hunter": "/ˈhʌntə/",
    "bag": "/bæɡ/", "life": "/laɪf/", "story": "/ˈstɔːri/",
    "tie": "/taɪ/", "never": "/ˈnevə/", "trust": "/trʌst/",
    "bad": "/bæd/", "guy": "/ɡaɪ/",
}

UNIT_NAMES = {
    1: "Unit 1",
    2: "Unit 2",
    3: "Unit 3",
    4: "Unit 4",
    5: "Unit 5",
    6: "Unit 6",
    7: "Plays 戏剧板块",
}


def main():
    rows = list(csv.reader(SRC.open(encoding="utf-8")))
    header, body = rows[0], rows[1:]
    missing = []
    out = []
    for word, _ph, meaning, unit in body:
        ph = PHONETICS.get(word, "")
        if not ph:
            missing.append(word)
        out.append([word, ph, meaning, unit])

    if missing:
        raise SystemExit(f"缺少音标：{missing}")

    with DST.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(out)
    print(f"写入 {DST}：{len(out)} 词")
    from collections import Counter
    c = Counter(int(r[3]) for r in out)
    for u, n in sorted(c.items()):
        print(f"  {UNIT_NAMES.get(u, u)}: {n} 词")
    phrases = sum(1 for r in out if " " in r[0])
    print(f"其中词组 {phrases} 个")


if __name__ == "__main__":
    main()
